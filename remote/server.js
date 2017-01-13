"use strict";


module.exports = function(port) {
    port = port || 8081;
    const io = require('socket.io')(port);
    const DeviceHandle = require('..');
    const metadata = require('./metadata.json');
    
    console.log('Started listening at port', port);
    
    io.on('connection', function (socket) {
        let connections = {};
        console.log('Incoming connection.');
        
        socket.on('create_handle', function(path, enableWrite, objSize, minObjSize) {
            console.log('Creating handle:', path);
            
            if(connections[path]) {
                connections[path].close();
                delete connections[path];
            }
    
            if(typeof minObjSize == 'undefined') minObjSize = objSize;
    
            try {
                connections[path] = new DeviceHandle(path, enableWrite, objSize, minObjSize, (err, data) => {
                    if(err) socket.emit('read_error', path, err);
                    else socket.emit('read_data', path, data);
                });
            } catch(err) {
                socket.emit('read_error', path, err.stack);
            }
        });
        
        const blacklist = ['ioctl'];
        
        metadata.functions.forEach((name) => {
            if(blacklist.indexOf(name) >= 0) return;
            socket.on(name, function() {
                let args = Array.prototype.slice.apply(arguments);
                let path = args.shift();
                try {
                    connections[path][name].apply(connections[path], args);
                } catch(e) {
                    socket.emit('server_error', e && e.stack ? e.stack : e);
                }
            });
        });
        
        socket.on('ioctl', (path, dir, type, cmd, data) => {
            dir = DeviceHandle[dir];
            connections[path].ioctl(dir, type, cmd, data);
        });
        
        socket.on('disconnect', function () {
            //destruct all
            Object.keys(connections).forEach((path) => { connections[path].close(); })
        });
    });
};