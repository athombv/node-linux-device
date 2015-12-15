#include <nan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#endif

using namespace v8;

class DeviceHandle: public Nan::ObjectWrap {
  public:
    DeviceHandle() : abort(false), closeCB(NULL) {}

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
    FILE *fd;
    uint32_t object_size;
    bool abort;
    Nan::Callback *closeCB;
};

class DeviceReadWorker : public Nan::AsyncProgressWorker {
  public:
    DeviceReadWorker(DeviceHandle *deviceHandle_, v8::Local<v8::Function> callback_)
        : Nan::AsyncProgressWorker(new Nan::Callback(callback_)), deviceHandle(deviceHandle_), wait(false) { }

    ~DeviceReadWorker() {
        if(callback) delete callback;
    }

    void Execute(const AsyncProgressWorker::ExecutionProgress& progress) {
        uint8_t buffer[deviceHandle->object_size];
        size_t i = 0;
        ssize_t count = 0;
        while(!deviceHandle->abort && (count = read(fileno(deviceHandle->fd), &buffer[i], deviceHandle->object_size-i)) >= 0) {
            i += count;
            if(i < deviceHandle->object_size) continue;
            i = 0;
            wait = true;
            progress.Send((char*)buffer, deviceHandle->object_size);
            while(!deviceHandle->abort && wait ) usleep(500);
        }

        if(!deviceHandle->abort) {
            SetErrorMessage(strerror(errno));
        }

        FILE *tmpFile = deviceHandle->fd;
        deviceHandle->fd = NULL;

        if(tmpFile) fclose(tmpFile);

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
    DeviceWriteWorker(DeviceHandle *deviceHandle_, v8::Local<v8::Function> callback_, void* buffer_, size_t bufferSize_)
        : Nan::AsyncWorker(new Nan::Callback(callback_)), deviceHandle(deviceHandle_), bufferSize(bufferSize_) {
            buffer = new uint8_t[bufferSize_];
            memcpy(buffer, buffer_, bufferSize_);
        }

    ~DeviceWriteWorker() {
        if(callback) delete callback;
        if(buffer) delete[] buffer;
    }

    void Execute() {
        uint8_t *current = buffer;
        size_t length = 0;
        while(length < bufferSize) {
            ssize_t res = write(fileno(deviceHandle->fd), &current[length], bufferSize-length);
            if(res < 0 ) {
                SetErrorMessage(strerror(errno));
                length = bufferSize;
            } else {
                length += res;
            }
        }
    }

    void WorkComplete() {
        Nan::AsyncWorker::WorkComplete();
        deviceHandle->Finish();
    }

  private:
    DeviceHandle *deviceHandle;
    uint8_t *buffer;
    size_t bufferSize;
};


NAN_METHOD(DeviceHandle::New) {
    Nan::HandleScope scope;

    assert(info.IsConstructCall());

    if(info.Length() < 4 || !info[0]->IsString() || !info[1]->IsBoolean() || !info[2]->IsUint32() || !info[3]->IsFunction()) {
        return Nan::ThrowTypeError("Invalid parameter: DeviceHandle(string path, boolean enableWrite, positive number objectSize, function callback)");
    }

    std::string devPath(*v8::String::Utf8Value(info[0]));
    const char *devPathCString = devPath.c_str();

    const char *devMode = info[1]->ToBoolean()->Value() ? "a+" : "r";
    FILE *fd = fopen(devPathCString, devMode);
    if(!fd) {
        return Nan::ThrowError(strerror(errno));
    }

    DeviceHandle* self = new DeviceHandle();
    self->fd = fd;
    self->object_size = info[2]->Uint32Value();

    self->Wrap(info.This());

    DeviceReadWorker* readWorker = new DeviceReadWorker(self, info[3].As<Function>());
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

    if(info.Length() < 2 || !info[0]->IsObject() || !info[1]->IsFunction()) {
        return Nan::ThrowTypeError("Invalid parameter: DeviceHandle.write( string path )");
    }

    Local<Object> bufferObj = info[0]->ToObject();
    char*  bufferData   = node::Buffer::Data(bufferObj);
    size_t bufferLength = node::Buffer::Length(bufferObj);

    self->Ref();

    DeviceWriteWorker* writeWorker = new DeviceWriteWorker(self, info[1].As<Function>(), bufferData, bufferLength);
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

    if(ioctl(fileno(self->fd), _IOC(direction, type, cmd, bufferLength), bufferData) == -1) {
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

    if(ioctl(fileno(self->fd), cmd, bufferData) == -1) {
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

    FILE * tmpFile = self->fd;
    self->fd = NULL;
    self->abort = true;

    fclose(tmpFile);

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
}

NODE_MODULE(DeviceHandle, setupModule)