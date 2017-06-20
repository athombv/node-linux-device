#include <nan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#endif

#define READ_TIMEOUT 2000

using namespace v8;

bool device_abort = false;

void device_exit_handler()
{
    device_abort = true;
}

class UVLockGuard {
  public:
    UVLockGuard(uv_mutex_t *mutex) :_mutex(mutex) {
        uv_mutex_lock(mutex);
    }
    ~UVLockGuard() {
        uv_mutex_unlock(_mutex);
    }
  private:
    uv_mutex_t *_mutex;
};

class DeviceHandle: public Nan::ObjectWrap {
  public:
    DeviceHandle() : abort(false), closeCB(NULL) {
        uv_mutex_init(&write_mutex);
    }

    ~DeviceHandle() {
        uv_mutex_destroy(&write_mutex);
    }

    void Finish() {
        this->Unref();
    }

    static NAN_METHOD(New);
    static NAN_METHOD(Write);
    static NAN_METHOD(Close);
    static NAN_METHOD(IsClosed);
#ifdef __linux__
    static NAN_METHOD(IOctl);
    static NAN_METHOD(IOctlRaw);
#endif

    static Local<Function> GetV8TPL(){
        Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
        tpl->SetClassName(Nan::New("DeviceHandle").ToLocalChecked());
        tpl->InstanceTemplate()->SetInternalFieldCount(1);

        SetPrototypeMethod(tpl, "write", Write);
        SetPrototypeMethod(tpl, "close", Close);
        SetPrototypeMethod(tpl, "isClosed", IsClosed);

#ifdef __linux__
        SetPrototypeMethod(tpl, "ioctl", IOctl);
        SetPrototypeMethod(tpl, "ioctl_raw", IOctlRaw);
#endif

        Local<Function> result = tpl->GetFunction();

#ifdef __linux__
        Nan::Set(result, Nan::New("IOCTL_NONE").ToLocalChecked(), Nan::New<Number>( _IOC_NONE ));
        Nan::Set(result, Nan::New("IOCTL_READ").ToLocalChecked(), Nan::New<Number>( _IOC_READ ));
        Nan::Set(result, Nan::New("IOCTL_WRITE").ToLocalChecked(), Nan::New<Number>( _IOC_WRITE ));
        Nan::Set(result, Nan::New("IOCTL_RW").ToLocalChecked(), Nan::New<Number>( _IOC_READ | _IOC_WRITE ));
#endif

        return result;
    }
    int fd;
    uint32_t object_size;
    uint32_t min_object_size;
    bool abort;
    Nan::Callback *closeCB;
    uv_mutex_t write_mutex;
};

class DeviceReadWorker : public Nan::AsyncProgressWorker {
  public:
    DeviceReadWorker(DeviceHandle *deviceHandle_, v8::Local<v8::Function> callback_)
        : Nan::AsyncProgressWorker(new Nan::Callback(callback_)), deviceHandle(deviceHandle_), wait(false) { }

    ~DeviceReadWorker() {
        if(callback) delete callback;
    }

    void Execute(const Nan::AsyncProgressWorker::ExecutionProgress& progress) {
        uint8_t buffer[deviceHandle->object_size];
        size_t i = 0;
        ssize_t count = 0;
        struct pollfd file = {
            deviceHandle->fd, /* file descriptor */
            POLLIN,           /* requested events */
            0                 /* returned events */
        };
        while(  (   !device_abort
                 && ((count = poll(&file, 1, READ_TIMEOUT)) >= 0)
                 && (!deviceHandle->abort)
                 && (count == 0 || ((file.revents & POLLIN) == POLLIN && (count = read(deviceHandle->fd, &buffer[i], deviceHandle->object_size-i)) > 0))
                ) || (
                     errno == EINTR
                  && !device_abort)
        ) {
            if(errno == EINTR || count <= 0) continue;
            i += count;
            if(i < deviceHandle->min_object_size) continue;

            wait = true;
            progress.Send((char*)buffer, i);
            i = 0;
            while(!deviceHandle->abort && wait && !device_abort) usleep(500);
        }

        if(!deviceHandle->abort) {
            SetErrorMessage(strerror(errno));
        }

        int tmpFile = deviceHandle->fd;
        deviceHandle->fd = 0;

        if(tmpFile) close(tmpFile);

        deviceHandle->abort = true;
    }

    void HandleProgressCallback(const char *data, size_t data_size) {
        Nan::HandleScope scope;

        if(deviceHandle->abort) return;

        Local<Value> argv[2];
        argv[0] = Nan::Null();
        argv[1] = Nan::CopyBuffer(data, data_size).ToLocalChecked(); //transfers ownership.
        callback->Call(2, argv);

        wait = false;
    }

    void WorkComplete() {
        Nan::HandleScope scope;
        Nan::AsyncProgressWorker::WorkComplete();
        deviceHandle->Finish();
        if(deviceHandle->closeCB) {
            deviceHandle->closeCB->Call(0, NULL);
            delete deviceHandle->closeCB;
            deviceHandle->closeCB = NULL;
        }
    }

    void HandleOKCallback() {
        //do nothing...
    }

  private:
    DeviceHandle *deviceHandle;
    bool wait;
};

class DeviceWriteWorker : public Nan::AsyncWorker {
  public:
    DeviceWriteWorker(DeviceHandle *deviceHandle_, v8::Local<v8::Function> callback_, void* buffer_, size_t bufferSize_, size_t count_ = 1, size_t interval_ = 0)
        : Nan::AsyncWorker(new Nan::Callback(callback_)),
                    deviceHandle(deviceHandle_),
                    bufferSize(bufferSize_),
                    count(count_),
                    interval(interval_) {
            buffer = new uint8_t[bufferSize_];
            memcpy(buffer, buffer_, bufferSize_);
        }

    ~DeviceWriteWorker() {
        if(callback) delete callback;
        if(buffer) delete[] buffer;
    }

    void Execute() {
        UVLockGuard(&deviceHandle->write_mutex);
        uint8_t *current = buffer;
        do {
            size_t length = 0;
            while(length < bufferSize) {
                ssize_t res = write(deviceHandle->fd, &current[length], bufferSize-length);
                if(res < 0 ) {
                    SetErrorMessage(strerror(errno));
                    length = bufferSize;
                } else {
                    length += res;
                }
            }
            fsync(deviceHandle->fd);
            if(count > 1) usleep(interval);
        } while(--count);
    }

    void WorkComplete() {
        Nan::HandleScope scope;
        Nan::AsyncWorker::WorkComplete();
        deviceHandle->Finish();
    }

  private:
    DeviceHandle *deviceHandle;
    uint8_t *buffer;
    size_t bufferSize;
    size_t count;
    size_t interval;
};


NAN_METHOD(DeviceHandle::New) {
    Nan::HandleScope scope;

    assert(info.IsConstructCall());

    if(info.Length() < 4 || !info[0]->IsString() || !info[1]->IsBoolean() || !info[2]->IsUint32() || !(info[3]->IsFunction() || (info[3]->IsUint32() && info[4]->IsFunction()) )) {
        return Nan::ThrowTypeError("Invalid parameter: DeviceHandle(string path, boolean enableWrite, positive number objectSize, [positive number minimalObjectSize,] function callback)");
    }

    std::string devPath(*v8::String::Utf8Value(info[0]));
    const char *devPathCString = devPath.c_str();

    int devMode = info[1]->ToBoolean()->Value() ? O_RDWR : O_RDONLY;
    int fd = open(devPathCString, devMode);
    if(fd < 0) {
        return Nan::ThrowError(strerror(errno));
    }

    DeviceHandle* self = new DeviceHandle();
    self->fd = fd;
    self->object_size = self->min_object_size = info[2]->Uint32Value();
    v8::Local<v8::Function> cb;

    if(info[3]->IsUint32()) {
        cb = info[4].As<Function>();
        self->min_object_size = info[3]->Uint32Value();
    } else {
        cb = info[3].As<Function>();
    }

    self->Wrap(info.This());

    DeviceReadWorker* readWorker = new DeviceReadWorker(self, cb);
    Nan::AsyncQueueWorker(readWorker);

    self->Ref();

    info.GetReturnValue().Set(info.This());
}

NAN_METHOD(DeviceHandle::Write) {
    Nan::HandleScope scope;

    DeviceHandle* self = ObjectWrap::Unwrap<DeviceHandle>(info.This());

    if(self->abort) {
        return Nan::ThrowError("Cannot write to closed DeviceHandle");
    }

    if(info.Length() < 2 || !info[0]->IsObject() || !info[info.Length()-2]->IsObject() || !info[info.Length()-1]->IsFunction()) {
        return Nan::ThrowTypeError("Invalid parameter: DeviceHandle.write( Buffer data[, Object opts], function callback )");
    }

    Local<Object> bufferObj = info[0]->ToObject();
    char*  bufferData   = node::Buffer::Data(bufferObj);
    size_t bufferLength = node::Buffer::Length(bufferObj);

    size_t repetitions = 1;
    size_t interval = 0;
    Local<Object> opts = info[info.Length()-2]->ToObject();
    Local<Value> repObj = opts->Get(Nan::New("repetitions").ToLocalChecked());
    if(repObj->IsNumber()) {
        repetitions = repObj->Uint32Value();
    }

    Local<Value> intvlObj = opts->Get(Nan::New("interval").ToLocalChecked());
    if(intvlObj->IsNumber()) {
        interval = intvlObj->Uint32Value();
    }

    self->Ref();

    DeviceWriteWorker* writeWorker = new DeviceWriteWorker(self, info[1].As<Function>(), bufferData, bufferLength, repetitions, interval);
    Nan::AsyncQueueWorker(writeWorker);

    info.GetReturnValue().Set(info.This());
}

#ifdef __linux__
NAN_METHOD(DeviceHandle::IOctl) {
    Nan::HandleScope scope;

    DeviceHandle* self = ObjectWrap::Unwrap<DeviceHandle>(info.This());

    if(self->abort) {
        return Nan::ThrowError("Cannot ioctl closed DeviceHandle");
    }

    if(info.Length() < 3 || !info[0]->IsNumber() || !info[1]->IsNumber() || !info[2]->IsNumber() || (info.Length() > 3 && !info[3]->IsObject())) {
        return Nan::ThrowError("Invalid parameter type: DeviceHandle.ioctl( int direction, int type, int cmd [, buffer data])");
    }

    uint32_t direction = info[0]->Uint32Value();
    uint32_t type = info[1]->Uint32Value();
    uint32_t cmd = info[2]->Uint32Value();
    void*  bufferData = NULL;
    size_t bufferLength = 0;

    if( (direction != _IOC_READ && direction != _IOC_WRITE && direction != (_IOC_READ | _IOC_WRITE) && direction != _IOC_NONE) || direction > _IOC_DIRMASK) {
        return Nan::ThrowTypeError("Invalid direction. Use DeviceHandle.IOCTL_RW or DeviceHandle.IOCTL_READ or DeviceHandle.IOCTL_WRITE or DeviceHandle.IOCTL_NONE");
    }

    if(type > _IOC_TYPEMASK) {
        return Nan::ThrowError("Invalid type.");
    }

    if(cmd > _IOC_NRMASK) {
        return Nan::ThrowError("Invalid cmd.");
    }

    if(bufferLength > _IOC_SIZEMASK) {
        return Nan::ThrowError("Invalid buffer size.");
    }

    if(info.Length() > 3 && info[3]->IsObject()) {
        Local<Object> bufferObj = info[3]->ToObject();
        bufferData   = node::Buffer::Data(bufferObj);
        bufferLength = node::Buffer::Length(bufferObj);
    }

    if(ioctl(self->fd, _IOC(direction, type, cmd, bufferLength), bufferData) == -1) {
        return Nan::ThrowError(strerror(errno));
    }

    info.GetReturnValue().Set(info.This());
}

NAN_METHOD(DeviceHandle::IOctlRaw) {
    Nan::HandleScope scope;

    DeviceHandle* self = ObjectWrap::Unwrap<DeviceHandle>(info.This());

    if(self->abort) {
        return Nan::ThrowError("Cannot ioctl closed DeviceHandle");
    }

    if(info.Length() < 1 || !info[0]->IsNumber() || (info.Length() > 1 && !info[1]->IsObject())) {
        return Nan::ThrowTypeError("Invalid parameter type: DeviceHandle.ioctl_raw( int cmd [, buffer data] )");
    }

    uint32_t cmd = info[0]->Uint32Value();
    void*  bufferData = NULL;

    if(info.Length() > 1 && info[1]->IsObject()) {
        Local<Object> bufferObj = info[1]->ToObject();
        bufferData = node::Buffer::Data(bufferObj);
    }

    if(ioctl(self->fd, cmd, bufferData) == -1) {
        return Nan::ThrowError(strerror(errno));
    }

    info.GetReturnValue().Set(info.This());
}
#endif

NAN_METHOD(DeviceHandle::Close) {
    Nan::HandleScope scope;

    DeviceHandle* self = ObjectWrap::Unwrap<DeviceHandle>(info.This());
    if(self->abort) return;

    if(info[0]->IsFunction()) {
        self->closeCB = new Nan::Callback(info[0].As<Function>());
    }

    self->abort = true;

    info.GetReturnValue().Set(info.This());
}

NAN_METHOD(DeviceHandle::IsClosed) {
    Nan::HandleScope scope;

    DeviceHandle* self = ObjectWrap::Unwrap<DeviceHandle>(info.This());

    info.GetReturnValue().Set(Nan::New<Boolean>(self->abort));
}


/**
 * exposes the functions through the module exports
 **/
NAN_MODULE_INIT(setupModule) {
    Local<Function> deviceHandleConstructor = DeviceHandle::GetV8TPL();

    Nan::Set(target, Nan::New("DeviceHandle").ToLocalChecked(), deviceHandleConstructor);
    std::atexit(device_exit_handler);
}

NODE_MODULE(DeviceHandle, setupModule)
