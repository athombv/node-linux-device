"use strict";
const io = require('socket.io-client');
const metadata = require('./metadata.json');

let server;
let handles = {};

class DeviceHandleProxy {
    constructor(path) {
        let args = Array.prototype.slice.apply(arguments);
        handles[path] = this;
        this._callback = args.pop();
        this._path = path;
        this._closed = false;
        args.unshift('create_handle');
        server = server.then(serv => {
            serv.emit.apply(serv, args);
            return serv;
        });
    }
    
    isClosed() {
        return this._closed;
    }
}

module.exports = function(url) {
    server = new Promise((resolve, reject) => {
        let serv = io.connect(url);
        
        serv.on('connect', () => resolve(serv));

        serv.on('read_error', function(path, err) {
            if(handles[path]) handles[path]._callback(err);
        });
    
        serv.on('read_data', (path, data) => {
            if(handles[path]) handles[path]._callback(null, data);
        });
        
        serv.on('server_error', (error) => {
            console.error('[linux-device] An remote error occured:', error);
        });
    
        serv.on('connect', () => console.log('REMOTE DEVICE SUPPORT ENABLED, CONNECTED TO:', url));
    
        serv.on('disconnect', () => {
            Object.keys(handles).forEach((handle) => {
                handles[handle]._closed = true;
                handles[handle]._callback(new Error('Remote Connection unexpectedly closed.'));
            });
        });
    });

    metadata.functions.forEach((func) => {
        DeviceHandleProxy.prototype[func] = function() {
            server = server.then(serv => serv.emit.bind(serv, func, this._path).apply(null, arguments));
            if(func == 'close') this._closed = true;
        };
    });

    metadata.static_functions.forEach((func) => {
        DeviceHandleProxy[func] = function() {
            server = server.then(serv => serv.emit.bind(serv, 'static_'+func).apply(null, arguments));
        };
    });

    metadata.constants.forEach((c, index) => {
        DeviceHandleProxy[c] = c;
    });

    return DeviceHandleProxy;
};
