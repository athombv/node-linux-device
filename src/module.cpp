#include <node_api.h>
#include <uv.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string>
#include <strings.h>
#include <poll.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <errno.h>
#endif

#define NAPI_ASSERT_STATUS(status) if(status != napi_ok) do {fprintf(stderr, "NAPI ASSERTION FAILED [%s@%d]: %d\n", __FUNCTION__, __LINE__, status ); abort();} while(0)


class FileHandle {
  public:
	static napi_value Init(napi_env env);
	static napi_value JSConstructor(napi_env env, napi_callback_info info);
	static void JSDestructor(napi_env env, void* data, void* hint);

	static napi_value Open(napi_env env, napi_callback_info info);
	static napi_value Close(napi_env env, napi_callback_info info);
	static napi_value Read(napi_env env, napi_callback_info info);
	static napi_value StopReading(napi_env env, napi_callback_info info);
	static napi_value Write(napi_env env, napi_callback_info info);
	static napi_value WriteRepeated(napi_env env, napi_callback_info info);
#ifdef __linux__
	static napi_value IOCtl(napi_env env, napi_callback_info info);
	static napi_value IOCtlRaw(napi_env env, napi_callback_info info);
#endif

	int getFD() { return this->_fd; };

  protected:
	static void OnRead(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf);
	static void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
	static void RequestResult(uv_fs_t *req);
	static void WorkResult(uv_work_t *req, int status);

  private:
	std::string _path;
	bool _reading;
	int _fd;

	uv_pipe_t *_file_pipe;
};

struct repeated_write_data {
	uv_buf_t buf;
	size_t interval;
	size_t repetitions;
};

struct fs_req_params {
	napi_env env;
	FileHandle *handle;
	uv_any_req req;
	void *data;
	napi_ref owner;
	napi_async_context context;
	napi_deferred deferred;
};

fs_req_params *createFSRequest(napi_env env, FileHandle *handle, napi_value owner, napi_value *promise) {
	napi_status status;
	fs_req_params *params;
	napi_value resource, resourceName;
	napi_handle_scope scope;

	params = new fs_req_params;
	params->env = env;
	params->handle = handle;

	bzero(&params->req, sizeof(uv_fs_t));
	params->req.req.data = params;

	status = napi_open_handle_scope(env, &scope);
	NAPI_ASSERT_STATUS(status);
	status = napi_create_reference(env, owner, 1, &params->owner);
	NAPI_ASSERT_STATUS(status);

	if(promise) {
		status = napi_create_promise(env, &params->deferred, promise);
		NAPI_ASSERT_STATUS(status);
	}

	status = napi_create_string_utf8(env, "FileHandle", NAPI_AUTO_LENGTH, &resource);
	NAPI_ASSERT_STATUS(status);
	status = napi_create_string_utf8(env, "FileHandle", NAPI_AUTO_LENGTH, &resourceName);
	NAPI_ASSERT_STATUS(status);

	status = napi_async_init(env, resource, resourceName, &params->context);
	NAPI_ASSERT_STATUS(status);
	status = napi_close_handle_scope(env, scope);
	NAPI_ASSERT_STATUS(status);

	return params;
}

napi_status destroyFSRequest(fs_req_params *params) {
	napi_status status;
	napi_handle_scope scope = NULL;
	status = napi_open_handle_scope(params->env, &scope);
	NAPI_ASSERT_STATUS(status);
	status = napi_delete_reference(params->env, params->owner);
	NAPI_ASSERT_STATUS(status);
	status = napi_async_destroy(params->env, params->context);
	NAPI_ASSERT_STATUS(status);
	status = napi_close_handle_scope(params->env, scope);
	NAPI_ASSERT_STATUS(status);
	if(params->req.req.type == UV_FS)
		uv_fs_req_cleanup(&params->req.fs);
	delete params;
	return status;
}

napi_status makeUVError(napi_env env, int code, napi_value *result) {
	napi_status status;
	napi_value jsCode;
	napi_value msg;
	status = napi_create_string_utf8(env, uv_err_name(code), NAPI_AUTO_LENGTH, &jsCode);
	NAPI_ASSERT_STATUS(status);
	status = napi_create_string_utf8(env, uv_strerror(code), NAPI_AUTO_LENGTH, &msg);
	NAPI_ASSERT_STATUS(status);
	return napi_create_error(env, jsCode, msg, result);
}



#define NAPI_DECLARE_METHOD(properties, name, m, len)  do { properties[*len].utf8name = name; properties[*len].method = m; (*len) ++; } while(0)
#define NAPI_DECLARE_STATIC_VALUE(properties, name, v, len)	 do { properties[*len].utf8name = name; properties[*len].value = v; properties[*len].attributes = napi_static; (*len) ++; } while(0)

napi_value FileHandle::Init(napi_env env) {
	napi_value result, constants;
	napi_create_object(env, &constants);

#ifdef __linux__
	napi_create_uint32(env, _IOC_NONE, &result);
	napi_set_named_property(env, constants, "IOCTL_NONE", result);
	napi_create_uint32(env, _IOC_READ, &result);
	napi_set_named_property(env, constants, "IOCTL_READ", result);
	napi_create_uint32(env, _IOC_WRITE, &result);
	napi_set_named_property(env, constants, "IOCTL_WRITE", result);
	napi_create_uint32(env, _IOC_READ | _IOC_WRITE, &result);
	napi_set_named_property(env, constants, "IOCTL_RW", result);
#endif

	napi_property_descriptor properties[9] = {};
	size_t property_length = 0;
	NAPI_DECLARE_METHOD(properties, "open", FileHandle::Open, &property_length);
	NAPI_DECLARE_METHOD(properties, "close", FileHandle::Close, &property_length);
	NAPI_DECLARE_METHOD(properties, "read", FileHandle::Read, &property_length);
	NAPI_DECLARE_METHOD(properties, "stopReading", FileHandle::StopReading, &property_length);
	NAPI_DECLARE_METHOD(properties, "write", FileHandle::Write, &property_length);
	NAPI_DECLARE_METHOD(properties, "writeRepeated", FileHandle::WriteRepeated, &property_length);
	NAPI_DECLARE_STATIC_VALUE(properties, "constants", constants, &property_length);
#ifdef __linux__
	NAPI_DECLARE_METHOD(properties, "ioctl", FileHandle::IOCtl, &property_length);
	NAPI_DECLARE_METHOD(properties, "ioctlRaw", FileHandle::IOCtlRaw, &property_length);
#endif

	napi_define_class(env, "FileHandle", NAPI_AUTO_LENGTH,
		FileHandle::JSConstructor, NULL, property_length, properties,
		&result);
	return result;
}

napi_value FileHandle::JSConstructor(napi_env env, napi_callback_info info) {
	napi_value jsThis, path;
	size_t argc = 1;
	FileHandle *cThis;
	char path_str[2048];

	napi_get_cb_info(env, info, &argc, &path, &jsThis, NULL);

	cThis = new FileHandle;
	napi_get_value_string_utf8(env, path, path_str, sizeof(path_str), NULL);
	cThis->_path = path_str;
	cThis->_fd = 0;
	cThis->_file_pipe = NULL;
	cThis->_reading = false;
	napi_wrap(env, jsThis, cThis, FileHandle::JSDestructor, NULL, NULL);
	return jsThis;
}

void FileHandle::JSDestructor(napi_env env, void* data, void* hint) {
	FileHandle *cThis = (FileHandle*)data;
	if(cThis->_file_pipe) {
		uv_close((uv_handle_t*)cThis->_file_pipe, NULL);
		cThis->_file_pipe = NULL;
	}
	delete cThis;
}

static void FreeBufferCB(napi_env env, void* finalize_data, void* buffer) {
	free(finalize_data);
}

void FileHandle::OnRead(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
	fs_req_params *params = (fs_req_params *)handle->data;
	napi_value readCb = NULL, argv[2], jsThis = NULL;
	napi_handle_scope scope = NULL;
	napi_env env; //params might be freed by make_callback
	size_t argc = 1;

	env = params->env;

	napi_open_handle_scope(env, &scope);

	napi_get_undefined(env, &argv[0]);

	if(nread <= 0 && buf->base != NULL) {
		free(buf->base);
	}

	if(nread < 0) {
		makeUVError(env, nread, &argv[0]);
		params->handle->_reading = false;
	} else if(nread == 0) {
		napi_get_null(env, &argv[1]);
		argc = 2;
	} else if(nread > 0) {
		void* base = realloc(buf->base, nread);
		napi_create_external_buffer(env, nread, base, FreeBufferCB, NULL, &argv[1]);
		argc = 2;
	}

	napi_get_reference_value(env, params->owner, &readCb);
	napi_create_object(env, &jsThis);
	napi_make_callback(env, params->context, jsThis, readCb, argc, argv, NULL);
	napi_close_handle_scope(env, scope);
}

void FileHandle::OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	buf->base = (char*)malloc(suggested_size);
	buf->len = suggested_size;
}

napi_value FileHandle::Read(napi_env env, napi_callback_info info) {
	napi_value jsThis, cb, err;
	size_t size = 1;
	FileHandle *cThis;
	int result;
	fs_req_params *params;

	cb = NULL;
	napi_get_cb_info(env, info, &size, &cb, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	napi_valuetype type = napi_undefined;
	napi_typeof(env, cb, &type);
	if(type != napi_function) {
		napi_throw_type_error(env, NULL, "Expected a function");
		return jsThis;
	}

	if(cThis->_fd > 0 && !cThis->_reading) {
		cThis->_reading = true;
		params = createFSRequest(env, cThis, cb, NULL);
		if(!cThis->_file_pipe) {
			cThis->_file_pipe = new uv_pipe_t;
			cThis->_file_pipe->data = params;

			uv_pipe_init(uv_default_loop(), cThis->_file_pipe, 0);
			uv_pipe_open(cThis->_file_pipe, cThis->_fd);
		}
		result = uv_read_start((uv_stream_t*)cThis->_file_pipe, OnAlloc, OnRead);
		if(result < 0) {
			makeUVError(env, result, &err);
			napi_call_function(env, jsThis, cb, 1, &err, NULL);
			destroyFSRequest(params);
		}
	}

	return jsThis;
}


napi_value FileHandle::StopReading(napi_env env, napi_callback_info info) {
	napi_value jsThis;
	FileHandle *cThis;
	napi_get_cb_info(env, info, 0, NULL, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);
	fs_req_params *params;
	if(cThis->_file_pipe) {
		params = (fs_req_params *)cThis->_file_pipe->data;
		uv_read_stop((uv_stream_t*)cThis->_file_pipe);
		destroyFSRequest(params);
	}
	cThis->_reading = false;
	return jsThis;
}

void FileHandle::RequestResult(uv_fs_t *req) {
	napi_handle_scope scope = NULL;
	napi_value result = NULL;
	fs_req_params *params = static_cast<fs_req_params*>(req->data);

	napi_open_handle_scope(params->env, &scope);

	if (req->result < 0) {
		// An error happened.
		makeUVError(params->env, req->result, &result);

		napi_reject_deferred(params->env, params->deferred, result);
	} else {
		switch (req->fs_type) {
		  case UV_FS_OPEN:
			params->handle->_fd = req->result;
			napi_create_int32(params->env, req->result, &result);
			break;
		  case UV_FS_CLOSE:
			if(params->handle->_file_pipe) {
				uv_close((uv_handle_t*)params->handle->_file_pipe, NULL);
				params->handle->_file_pipe = NULL;
			}
			params->handle->_fd = 0;
			napi_create_int32(params->env, req->result, &result);
			break;
		  case UV_FS_WRITE:
			napi_create_int32(params->env, req->result, &result);
			break;
		  default:
			napi_get_undefined(params->env, &result);
			fprintf(stderr, "[linux-device] Got RequestResult for unimplemented method????\n");
			break;
		}

		napi_resolve_deferred(params->env, params->deferred, result);
	}

	napi_close_handle_scope(params->env, scope);

	destroyFSRequest(params);

	return;
}

void FileHandle::WorkResult(uv_work_t* req, int status) {
	fs_req_params *params = static_cast<fs_req_params*>(req->data);
	params->req.fs.fs_type = UV_FS_WRITE;
	FileHandle::RequestResult(&params->req.fs);
}

void repeated_write_work(uv_work_t* req) {
	fs_req_params *params = static_cast<fs_req_params*>(req->data);
	struct repeated_write_data *data = static_cast<struct repeated_write_data *>(params->data);
	size_t repetitions = data->repetitions;
	while(repetitions--) {
		params->req.fs.result += write(params->handle->getFD(), data->buf.base, data->buf.len);
		if(repetitions) usleep(data->interval);
	}
}

napi_value FileHandle::WriteRepeated(napi_env env, napi_callback_info info) {
	napi_value jsThis, argv[3], err, promise;
	FileHandle *cThis;
	size_t argc = 3;
	int result;
	fs_req_params *params;
	repeated_write_data *data;

	napi_get_cb_info(env, info, &argc, argv, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	params = createFSRequest(env, cThis, jsThis, &promise);
	data = (repeated_write_data*)malloc(sizeof(struct repeated_write_data));
	params->data = data;

	//this buffer is retained by js
	napi_get_buffer_info(env, argv[0], (void**)&data->buf.base, &data->buf.len);

	uint32_t interval, repetitions;
	napi_get_value_uint32(env, argv[1], &interval);
	napi_get_value_uint32(env, argv[2], &repetitions);
	data->interval = interval;
	data->repetitions = repetitions;

	result = uv_queue_work(uv_default_loop(), &params->req.work, repeated_write_work, FileHandle::WorkResult);

	if(result < 0) {
		makeUVError(env, result, &err);
		napi_reject_deferred(env, params->deferred, err);
		destroyFSRequest(params);
	}

	return promise;
}

napi_value FileHandle::Write(napi_env env, napi_callback_info info) {
	napi_value jsThis, data, err, promise;
	FileHandle *cThis;
	size_t argc = 1;
	int result;
	fs_req_params *params;
	void *buf = NULL;
	size_t len = 0;

	napi_get_cb_info(env, info, &argc, &data, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	params = createFSRequest(env, cThis, jsThis, &promise);

	napi_get_buffer_info(env, data, &buf, &len);

	//this buffer is retained by js
	uv_buf_t buffer = uv_buf_init((char*)buf, len);

	result = uv_fs_write(uv_default_loop(), &params->req.fs, cThis->_fd, &buffer, 1, -1, RequestResult);

	if(result < 0) {
		makeUVError(env, result, &err);
		napi_reject_deferred(env, params->deferred, err);
		destroyFSRequest(params);
	}

	return promise;
}

napi_value FileHandle::Open(napi_env env, napi_callback_info info) {
	napi_value jsThis, mode, err, promise;
	FileHandle *cThis;
	uint32_t c_flags;
	int result;
	size_t argc = 1;
	fs_req_params *params;

	napi_get_cb_info(env, info, &argc, &mode, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	params = createFSRequest(env, cThis, jsThis, &promise);

	napi_get_value_uint32(env, mode, &c_flags);

	result = uv_fs_open(uv_default_loop(), &params->req.fs, cThis->_path.c_str(), c_flags, 0, RequestResult);

	if(result < 0) {
		makeUVError(env, result, &err);
		napi_reject_deferred(env, params->deferred, err);
		destroyFSRequest(params);
	}

	return promise;
}


napi_value FileHandle::Close(napi_env env, napi_callback_info info) {
	napi_value jsThis, err, promise;
	FileHandle *cThis;
	int result;
	fs_req_params *params;

	napi_get_cb_info(env, info, 0, NULL, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	StopReading(env, info);

	params = createFSRequest(env, cThis, jsThis, &promise);

	result = uv_fs_close(uv_default_loop(), &params->req.fs, cThis->_fd, RequestResult);

	if(result < 0) {
		makeUVError(env, result, &err);
		napi_reject_deferred(env, params->deferred, err);
		destroyFSRequest(params);
	}

	return promise;
}

#ifdef __linux__
napi_value FileHandle::IOCtl(napi_env env, napi_callback_info info) {
	napi_value jsThis, argv[4], err;
	FileHandle *cThis;
	size_t argc = 4;
	uint32_t direction = 0, type = 0, cmd = 0;
	void*  bufferData = NULL;
	size_t bufferLength = 0;

	napi_get_cb_info(env, info, &argc, argv, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	if(argc < 4) {
		napi_throw_type_error(env, NULL, "invalid_parameter");
		return jsThis;
	}

	napi_get_value_uint32(env, argv[0], &direction);
	napi_get_value_uint32(env, argv[1], &type);
	napi_get_value_uint32(env, argv[2], &cmd);


	if( (direction != _IOC_READ && direction != _IOC_WRITE && direction != (_IOC_READ | _IOC_WRITE) && direction != _IOC_NONE) || direction > _IOC_DIRMASK) {
		napi_throw_type_error(env, NULL, "Invalid direction. Use DeviceHandle.constants.IOCTL_RW or DeviceHandle.constants.IOCTL_READ or DeviceHandle.constants.IOCTL_WRITE or DeviceHandle.constants.IOCTL_NONE");
		return jsThis;
	}

	if(type > _IOC_TYPEMASK) {
		napi_throw_type_error(env, NULL, "invalid_type");
		return jsThis;
	}

	if(cmd > _IOC_NRMASK) {
		napi_throw_type_error(env, NULL, "invalid_cmd");
		return jsThis;
	}

	if(argc > 3) {
		napi_get_buffer_info(env, argv[3], &bufferData, &bufferLength);
		if(bufferLength > _IOC_SIZEMASK) {
			napi_throw_type_error(env, NULL, "invalid_buffer_size");
			return jsThis;
		}
	}

	if(ioctl(cThis->_fd, _IOC(direction, type, cmd, bufferLength), bufferData) == -1) {
		makeUVError(env, errno, &err);
		napi_throw(env, err);
	}

	return jsThis;
}

napi_value FileHandle::IOCtlRaw(napi_env env, napi_callback_info info) {
	napi_value jsThis, argv[2], err;
	FileHandle *cThis;
	size_t argc = 2;
	uint32_t cmd = 0;
	void*  bufferData = NULL;
	size_t bufferLength = 0;

	napi_get_cb_info(env, info, &argc, argv, &jsThis, NULL);
	napi_unwrap(env, jsThis, (void**)&cThis);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return jsThis;

	if(argc < 2) {
		napi_throw_type_error(env, NULL, "invalid_parameter");
		return jsThis;
	}

	napi_get_value_uint32(env, argv[0], &cmd);

	if(!cmd) {
		napi_throw_type_error(env, NULL, "invalid_cmd");
		return jsThis;
	}

	napi_get_buffer_info(env, argv[1], &bufferData, &bufferLength);
	if(bufferLength > _IOC_SIZEMASK) {
		napi_throw_type_error(env, NULL, "invalid_buffer_size");
		return jsThis;
	}

	if(ioctl(cThis->_fd, cmd, bufferData) == -1) {
		makeUVError(env, errno, &err);
		napi_throw(env, err);
	}

	return jsThis;
}
#endif

napi_value Init(napi_env env, napi_value result) {
	return FileHandle::Init(env);
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init);