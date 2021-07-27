"use strict";

const {Duplex} = require('stream');
const fs = require('fs');
const util = require('util');

const io = require('socket.io-client');
const metadata = require('./metadata.json');

let server;
let handles = {};

let CONSTANTS = Object.assign({}, fs.constants);

let i = 0;

function makeRemoteFunc(...meta) {
	return async function(...args) {
		server = await server;
		try {
			if(this._currentCall) await this._currentCall;
		} catch(e) {}
		const work = new Promise((resolve, reject) => {
			function done(err, res) {
				//console.log('done', ...meta, err, res);
				if(err) return reject(err);
				resolve(res);
			}
			//console.log('executing', ...meta, ...args);
			args.push(done);
			if(this && this._handle)
				server.emit(...meta, this._handle, ...args);
			else
				server.emit(...meta, ...args);
		});
		this._currentCall = work;
		try {
		    await work;
		} catch(e) {}
		if(this._currentCall === work) this._currentCall = null;
		return work;
	};
}

class DeviceHandleProxy extends Duplex {
	constructor(options) {
		super(options);
		this._handle = ++i;
		handles[i] = this;
		this.__init(options);
	}

	isOpen() {
		return !!this.fd;
	}

	_write(chunk, encoding, callback) {
    // Custom parsing for IR header
    let repetitions;
    let interval;
    const irHeaderMarker = [0x69, 0x72];
    if (chunk[0] === irHeaderMarker[0] && chunk[1] === irHeaderMarker[1]
      && chunk[5] === irHeaderMarker[0] && chunk[6] === irHeaderMarker[1]) {
      repetitions = chunk[2];
      interval = Buffer.from(chunk).readUInt16BE(3);
      chunk = chunk.slice(7)
    }
		this._writeChunk(chunk, {gap: interval, repetitions})
			.then(res => callback(null, res)).catch(err => callback(err));
	}

	_writePromise(chunk, encoding) {
		return this._writeChunk(chunk, {gap: chunk.gap, repetitions: chunk.repetitions});
	}

	_read(size) {
		this._startRead(size);
	}

	_destroy(error, callback) {
		this.close()
			.then(res => callback(null, res)).catch(err => callback(err));
		if(error) this.emit('error', error);
	}

	_handle_open(fd) {
		this.fd = fd;
		this.emit('open', fd);
	}

	_handle_close() {
		delete this.fd;
		this.emit('close');
	}

	_handle_data(data) {
		this.push(Buffer.from(data))
	}

	static get constants() {
		return CONSTANTS;
	}
}

DeviceHandleProxy.prototype.__init = makeRemoteFunc('create_handle');

const functions = metadata.functions.concat(metadata.internal_functions);
functions.forEach((func) => {
		DeviceHandleProxy.prototype[func] = makeRemoteFunc('function', func);
});

metadata.static_functions.forEach((func) => {
	DeviceHandleProxy[func] = makeRemoteFunc('static_func', func);
});


module.exports = function(url) {
	server = new Promise((resolve, reject) => {
		let serv = io.connect(url, {transports: ['websocket']});

		serv.on('connect', () => resolve(serv));
		serv.on('constants', (constants) => Object.assign(CONSTANTS, constants));

		serv.on('server_error', (error) => {
			console.error('[linux-device] An remote error occured:', error);
		});

		serv.on('connect', () => console.log('[linux-device] REMOTE DEVICE SUPPORT ENABLED, CONNECTED TO:', url));

		serv.on('handle_event', (handle, event, ...args) => {
			if(!handles[handle]) return;
			if(handles[handle]['_handle_'+event])
				handles[handle]['_handle_'+event](...args);
			else
				handles[handle].emit(event, ...args);
		});

		serv.on('disconnect', () => {
			Object.keys(handles).forEach((handle) => {
				handles[handle].destroy(new Error('Remote connection unexpectedly closed.'));
			});
		});
	});

	return DeviceHandleProxy;
};
