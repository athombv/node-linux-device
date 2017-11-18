"use strict";

const util = require('util');
const DeviceHandle = require('../lib/DeviceHandle.js');

class Handler {
    constructor(handle, socket, options) {
        this.socket = socket;
        this._handle = handle;
        this.handle = new DeviceHandle(options);
        this.__bindEvents();
    }
    
    __destruct() {
        this.handle.close();
    }
    
    __bindEvents() {
        const that = this;
        this.handle.emit = function(...args) {
            that.socket.emit('handle_event', that._handle, ...args);
            return this.constructor.prototype.emit.apply(this, args);
        }
    }
    
    async _handleFunction(func, ...args) {
        if(this._currentFunc) {
            await this._currentFunc;
        }
        
        var res;
        if(this[func]) res = this[func](...args);
        else res = this.handle[func](...args);
        
        this._currentFunc = res;
        await res;
        if(res === this._currentFunc) this._currentFunc = null;
        return res;
    }
    
    _writeChunk(chunk) {
        return this.handle._writePromise(chunk);
    }
    
    _writeChunks(chunks) {
        return this.handle._writevPromise(chunks);
    }
    
    _startRead(size) {
        if(this._isReading) return;
        this._isReading = true;
        this.handle.once('data', () => {
            this._isReading = false;
            process.nextTick(() => {
                this.handle.pause();
            });
        });
        this.handle.resume();
    }
}


module.exports = function(port) {
    port = port || 8081;
    const io = require('socket.io')(port);
    const metadata = require('./metadata.json');
    
    console.log('Started listening at port', port);
    
    io.on('connection', function (socket) {
        let connections = {};
        console.log('Incoming connection.');
        socket.emit('constants', DeviceHandle.constants);
        
        socket.on('create_handle', function(handle, options, cb) {
            console.log('Creating handle:', handle, options);
            
            if(connections[handle]) {
                connections[handle].__destruct();
                delete connections[handle];
            }
            connections[handle] = new Handler(handle, socket, options);
            cb();
        });
        
        
        async function handleFunction(func, handle, ...args) {
            const cb = args.pop();
            if(!connections[handle]) return cb();
            return connections[handle]._handleFunction(func, ...args)
                    .then(res => cb(null, res)).catch(err => cb(err.stack));
        }
        
        async function handleStaticFunction(func, ...args) {
            const cb = args.pop();
            return DH[func](...args)
                    .then(res => cb(null, res)).catch(err => cb(err.stack));
        }
        
        socket.on('function', handleFunction);
        socket.on('static_func', handleStaticFunction)
        
        socket.on('disconnect', function () {
            //destruct all
            Object.keys(connections).forEach((handle) => { connections[handle].__destruct(); });
        });
    });
};