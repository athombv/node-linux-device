"use strict";
const io = require('socket.io-client');
const metadata = require('./metadata.json');

let server;
let handles = {};

class DeviceHandleProxy {
    constructor(path) {
        let args = Array.prototype.slice.apply(arguments);
        handles[path] = args.pop();
        this._path = path;
        args.unshift('create_handle');
        server.then(serv => serv.emit.apply(serv, args));
    }
}

module.exports = function(url) {
    server = new Promise((resolve, reject) => {
        let serv = io.connect(url);
        
        serv.on('connect', () => resolve(serv));
        serv.on('connect_error', () => reject());
        serv.on('connect_timeout', () => reject());

        serv.on('read_error', function(path, err) {
            if(handles[path]) handles[path](err);
        });
    
        serv.on('read_data', (path, data) => {
            if(handles[path]) handles[path](null, data);
        });
        
        serv.on('server_error', (error) => {
            console.error('[linux-device] An remote error occured:', error);
        });
    
        serv.on('connect', () => console.log('REMOTE DEVICE SUPPORT ENABLED, CONNECTED TO:', url));
    
        serv.on('disconnect', () => {
            Object.keys(handles).forEach((handle) => {
                handles[handle](new Error('Remote Connection unexpectedly closed.'));
            });
        });
    });

    metadata.functions.forEach((func) => {
        DeviceHandleProxy.prototype[func] = function() {
            server.then(serv => serv.emit.bind(serv, func, this._path).apply(null, arguments));
        };
    });

    metadata.constants.forEach((c, index) => {
        DeviceHandleProxy[c] = c;
    });

    return DeviceHandleProxy;
};