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
	static napi_value WriteRepeated(napi_env env, napi_callback_info info);
#ifdef __linux__
	static napi_value IOCtl(napi_env env, napi_callback_info info);
	static napi_value IOCtlRaw(napi_env env, napi_callback_info info);
#endif

  protected:
	static void RequestResult(uv_fs_t *req);
	static void WorkResult(uv_work_t *req, int status);

};

struct repeated_write_data {
	int fd;
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

fs_req_params *createFSRequest(napi_env env, napi_value *promise) {
	napi_status status;
	fs_req_params *params;
	napi_value resource, resourceName;
	napi_handle_scope scope;

	params = new fs_req_params;
	params->env = env;

	bzero(&params->req, sizeof(uv_fs_t));
	params->req.req.data = params;

	status = napi_open_handle_scope(env, &scope);
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
	status = napi_async_destroy(params->env, params->context);
	NAPI_ASSERT_STATUS(status);
	status = napi_close_handle_scope(params->env, scope);
	NAPI_ASSERT_STATUS(status);
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

	napi_property_descriptor properties[5] = {};
	size_t property_length = 0;
	NAPI_DECLARE_METHOD(properties, "writeRepeated", FileHandle::WriteRepeated, &property_length);
	NAPI_DECLARE_STATIC_VALUE(properties, "constants", constants, &property_length);
#ifdef __linux__
	NAPI_DECLARE_METHOD(properties, "ioctl", FileHandle::IOCtl, &property_length);
	NAPI_DECLARE_METHOD(properties, "ioctlRaw", FileHandle::IOCtlRaw, &property_length);
#endif

	napi_create_object(env, &result);
	napi_define_properties(env, result, property_length, properties);
	return result;
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
		napi_create_int32(params->env, req->result, &result);
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
	free(params->data);
}


void repeated_write_work(uv_work_t* req) {
	fs_req_params *params = static_cast<fs_req_params*>(req->data);
	struct repeated_write_data *data = static_cast<struct repeated_write_data *>(params->data);
	size_t repetitions = data->repetitions;
	while(repetitions--) {
		params->req.fs.result += write(data->fd, data->buf.base, data->buf.len);
		if(repetitions) usleep(data->interval);
	}
}

napi_value FileHandle::WriteRepeated(napi_env env, napi_callback_info info) {
	napi_value argv[4], err, promise;
	size_t argc = 4;
	int result;
	fs_req_params *params;
	repeated_write_data *data;

	napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return NULL;

	params = createFSRequest(env, &promise);
	data = (repeated_write_data*)malloc(sizeof(struct repeated_write_data));
	params->data = data;

	//this buffer is retained by js

	uint32_t interval, repetitions;
	int fd;

	napi_get_value_int32(env, argv[0], &fd);
	napi_get_buffer_info(env, argv[1], (void**)&data->buf.base, &data->buf.len);
	napi_get_value_uint32(env, argv[2], &interval);
	napi_get_value_uint32(env, argv[3], &repetitions);
	data->fd = fd;
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


#ifdef __linux__
napi_value FileHandle::IOCtl(napi_env env, napi_callback_info info) {
	napi_value argv[5], err, undefined;
	size_t argc = 5;
	int fd;
	uint32_t direction = 0, type = 0, cmd = 0;
	void*  bufferData = NULL;
	size_t bufferLength = 0;

	napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
	napi_get_undefined(env, &undefined);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return undefined;

	if(argc < 5) {
		napi_throw_type_error(env, NULL, "invalid_parameter");
		return undefined;
	}

	napi_get_value_int32(env, argv[0], &fd);
	napi_get_value_uint32(env, argv[1], &direction);
	napi_get_value_uint32(env, argv[2], &type);
	napi_get_value_uint32(env, argv[3], &cmd);


	if( (direction != _IOC_READ && direction != _IOC_WRITE && direction != (_IOC_READ | _IOC_WRITE) && direction != _IOC_NONE) || direction > _IOC_DIRMASK) {
		napi_throw_type_error(env, NULL, "Invalid direction. Use DeviceHandle.constants.IOCTL_RW or DeviceHandle.constants.IOCTL_READ or DeviceHandle.constants.IOCTL_WRITE or DeviceHandle.constants.IOCTL_NONE");
		return undefined;
	}

	if(type > _IOC_TYPEMASK) {
		napi_throw_type_error(env, NULL, "invalid_type");
		return undefined;
	}

	if(cmd > _IOC_NRMASK) {
		napi_throw_type_error(env, NULL, "invalid_cmd");
		return undefined;
	}

	if(argc > 4) {
		napi_get_buffer_info(env, argv[4], &bufferData, &bufferLength);
		if(bufferLength > _IOC_SIZEMASK) {
			napi_throw_type_error(env, NULL, "invalid_buffer_size");
			return undefined;
		}
	}

	if(ioctl(fd, _IOC(direction, type, cmd, bufferLength), bufferData) == -1) {
		makeUVError(env, errno, &err);
		napi_throw(env, err);
	}

	return undefined;
}

napi_value FileHandle::IOCtlRaw(napi_env env, napi_callback_info info) {
	napi_value argv[3], err, undefined;
	size_t argc = 3;
	uint32_t cmd = 0;
	int fd;
	void*  bufferData = NULL;
	size_t bufferLength = 0;

	napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
	napi_get_undefined(env, &undefined);

	bool isPendingException = false;
	napi_is_exception_pending(env, &isPendingException);
	if(isPendingException) return undefined;

	if(argc < 3) {
		napi_throw_type_error(env, NULL, "invalid_parameter");
		return undefined;
	}

	napi_get_value_int32(env, argv[0], &fd);
	napi_get_value_uint32(env, argv[1], &cmd);

	if(!cmd) {
		napi_throw_type_error(env, NULL, "invalid_cmd");
		return undefined;
	}

	napi_get_buffer_info(env, argv[2], &bufferData, &bufferLength);
	if(bufferLength > _IOC_SIZEMASK) {
		napi_throw_type_error(env, NULL, "invalid_buffer_size");
		return undefined;
	}

	if(ioctl(fd, cmd, bufferData) == -1) {
		makeUVError(env, errno, &err);
		napi_throw(env, err);
	}

	return undefined;
}
#endif

napi_value Init(napi_env env, napi_value result) {
	return FileHandle::Init(env);
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init);