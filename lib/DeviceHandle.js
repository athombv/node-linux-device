"use strict";

const {Duplex} = require('stream');
const {EventEmitter} = require('events');
const util = require('util');
const _fs = require('fs');
const tty = require('tty');
const net = require('net');

const FH = require('bindings')('DeviceHandle');

const fs = {
    open: util.promisify(_fs.open),
    close: util.promisify(_fs.close),
    writeFile: util.promisify(_fs.writeFile),
    fstat: util.promisify(_fs.fstat),
    createReadStream: _fs.createReadStream,
	createWriteStream: _fs.createWriteStream,
	constants: _fs.constants,
};


const kSource = Symbol('source');

const SYSFS_GPIO = '/sys/class/gpio';

const MODEM_BITS = {
	"BIT_LE"	:	 0x001,
	"BIT_DTR"	:	 0x002,
	"BIT_RTS"	:	 0x004,
	"BIT_ST"	:	 0x008,
	"BIT_SR"	:	 0x010,
	"BIT_CTS"	:	 0x020,
	"BIT_CAR"	:	 0x040,
	"BIT_RNG"	:	 0x080,
	"BIT_DSR"	:	 0x100,
	"BIT_CD"	:	 0x040,
	"BIT_RI"	:	 0x080,
	"BIT_OUT1"	:	 0x2000,
	"BIT_OUT2"	:	 0x4000,
	"BIT_LOOP"	:	 0x8000
};

const CONSTANTS = Object.assign(fs.constants, FH.constants, MODEM_BITS);

const IOCTL_TIOCMBIS = 0x5416;
const IOCTL_TIOCMBIC = 0x5417;


/**
 * NodeJS Stream API
 * @external Stream
 * @see https://nodejs.org/api/stream.html
 */

/**
 * JS Representation of a device inode
 * @param {object} options
 * @param {string} options.path The path to the device
 * @param {Number} [options.mode] The mode in which to open this device, defaults to constants.O_RDWR
 * @param {Number} [options.absoluteSize] The object size of the read data, limits the buffer size to this size
 * @param {boolean} [options.autoOpen] Set to true to automatically open this device upon construction
 * @extends external:Stream
 */
class DeviceHandle extends Duplex {
	constructor(options) {
		super(options);
		options = options || {};
		if(typeof options === 'string') options = {path: options};
		const mode = options.mode || CONSTANTS.O_RDWR;
		this[kSource] = {
			path: options.path,
			mode: mode,
			readable: mode & CONSTANTS.O_RDONLY || mode & CONSTANTS.O_RDWR,
			writable: mode & CONSTANTS.O_WRONLY || mode & CONSTANTS.O_RDWR,
			parser: options.parser || undefined,
			reading: 0,
			forcedDataSize: options.absoluteSize,
		}
		if(options && options.autoOpen) this.open().catch(err => this.emit('error', err));
	}

	__onEnd(err) {
		if(err) this.close().catch(e => {});
	}

	__onError(err) {
		if(err) this.close().catch(e => {});
		this.emit(err);
	}

	__pushSmart(res) {
		let result = true;
		if(res && this[kSource].parser) {
			if(!this[kSource].emitter) {
				this[kSource].emitter = new EventEmitter();
				this[kSource].emitter.on('data', (data) => this.push(data));
			}
			this[kSource].parser(this[kSource].emitter, res);
		} if(res && this[kSource].forcedDataSize) {
			for(let i = 0; i < res.length; i+= this[kSource].forcedDataSize) {
				result = this.push(res.slice(i, i+this[kSource].forcedDataSize));
			}
		} else {
			result = this.push(res);
		}
		return result;
	}

    async _createStreams() {
        if(tty.isatty(this.fd)) {
			if(this[kSource].readable) {
				this[kSource].inStream = new tty.ReadStream(this.fd);
				this[kSource].inStream.setRawMode(true);
			}
			
			if(this[kSource].writable) {
				this[kSource].outStream = new tty.WriteStream(this.fd);
			}
        } else {
            const fstat = await fs.fstat(this.fd);
            if(fstat.isFile() || fstat.isCharacterDevice() || fstat.isBlockDevice() ) {

				if(this[kSource].readable)
					this[kSource].inStream = fs.createReadStream(null, {fd: this.fd, autoClose: false});
				
				if(this[kSource].writable)
                	this[kSource].outStream = fs.createWriteStream(null, {fd: this.fd, autoClose: false});
            } else if(fstat.isSocket()) {
                this[kSource].inStream = this[kSource].outStream
                    = new net.Socket({
                        fd: this.fd,
                        readable: this[kSource].readable,
                        writable: this[kSource].writable,
                    })
            } else {
                throw new Error("unknown_type");
            }
		}

		if(this[kSource].outStream && this[kSource].outStream != this[kSource].inStream) {
			this[kSource].outStream.on('error', err => this.__onError(err));
		}

		if(this[kSource].inStream) {
			this[kSource].inStream.on('error', err => this.__onError(err));
			this[kSource].inStream.on('end', err => this.__onEnd(err));
			this[kSource].inStream.pause();
			this[kSource].inStream.on('data', (data) => {
				if(!this.__pushSmart(data)) {
					this[kSource].inStream.pause();
				}
			});
			if(this[kSource].resumeOnCreate){
				delete this[kSource].resumeOnCreate;
				this[kSource].inStream.resume();
			}
		}
    }

	/**
	 * Opens the device
	 * @returns {number} Device fd
	 */
	async open() {
		if(this.fd) await this.close().catch(()=>{});
		if(this[kSource].tainted) throw new Error('attempt to reuse closed DeviceHandle');
		const res = await fs.open(this[kSource].path, this[kSource].mode);
		this.fd = res;
        await this._createStreams();
		this.emit('open', res);
		if(this[kSource].reading) {
			this._read(0);
		}
		return res;
	}

	/**
	 * Closes the device
	 */
	async close() {
        if(this[kSource].outStream) {
            this[kSource].outStream.cork();
            this[kSource].outStream.removeAllListeners();
            delete this[kSource].outStream;
        }
        if(this[kSource].inStream) {
            this[kSource].inStream.pause();
            this[kSource].inStream.removeAllListeners();
            delete this[kSource].inStream;
        }
		if(!this.fd) return;
		await fs.close(this.fd);
		delete this.fd;
		this.emit('close');
	}
	
	/**
	 * Checks if the device is open
	 * @returns {boolean} True when the device is open, false otherwise
	 */
	isOpen() {
		return !!this.fd;
	}
	
	/**
	 * Performs an ioctl on the device
	 * @param {number} direction - Either constants.IOCTL_NONE, IOCTL_READ, IOCTL_WRITE, IOCTL_RW
	 * @param {number} type - The ioctl type
	 * @param {number} cmd - The ioctl command
	 * @param {Buffer} data - The ioctl data, the data may be changed by the ioctl
	 */
	async ioctl(direction, type, cmd, data) {
		if(!this.fd) throw new Error('not_open');
		await FH.ioctl(this.fd, direction, type, cmd, data||Buffer.alloc(0));
	}
	
	/**
	 * Performs a raw ioctl on the device
	 * @param {number} cmd - The ioctl command
	 * @param {Buffer} data - The ioctl data, the data may be changed by the ioctl
	 */
	async ioctlRaw(cmd, data) {
		if(!this.fd) throw new Error('not_open');
		await FH.ioctlRaw(this.fd, cmd, data||Buffer.alloc(0));
	}

	async setModemBits(bits) {
		var data = Buffer.allocUnsafe(4);
		data.writeUInt32LE(bits, 0);
		return this.ioctlRaw(IOCTL_TIOCMBIS, data);
	}

	async clearModemBits(bits) {
		var data = Buffer.allocUnsafe(4);
		data.writeUInt32LE(bits, 0);
		return this.ioctlRaw(IOCTL_TIOCMBIC, data);
	};
	
	async flush() {
		console.log('[linux-device] Calling unimplemented flush method, invoking callback without doing anything.');
	};
		
		
	/**
	 * Alias for DeviceHandle.gpio
	 */
	async gpio(...args) {
		return await this.constructor.gpio(...args);
	}
	
	/**
	 * Sets a GPIO output pin
	 * @param {number} gpio - The gpio number
	 * @param {Buffer} value - Either true or false
	 */
	static async gpio(gpio, value) {
		await fs.writeFile( SYSFS_GPIO+'/export', ''+gpio).catch(e => {});
		await fs.writeFile(SYSFS_GPIO+'/gpio'+gpio+'/direction', value ? 'high': 'low');
	};
	
	
	/**
	 * DeviceHandle constants
	 */
	static get constants() {
		return CONSTANTS;
	}
	
	
	
	_write(chunk, encoding, callback) {
		util.callbackify(this._writePromise).apply(this, arguments);
	}

    _read(size) {
		if(this[kSource].inStream)
			this[kSource].inStream.resume();
		else
			this[kSource].resumeOnCreate = true;
    }

	async _writePromise(chunk, encoding) {
		if(!this.fd) throw new Error('not_open');
		if(!this[kSource].writable) throw new Error('not_writable');
		if(chunk.interval && chunk.repetitions) {
			return await FH.writeRepeated(this.fd, chunk, chunk.interval, chunk.repetitions); //retain chunks
		} else {
			return util.promisify(this[kSource].outStream.write).call(this[kSource].outStream, chunk, encoding); //retain chunk
		}
	}
	
	_destroy(error, callback) {
		if(!this.fd) return;
		util.callbackify(this.close).call(this, (err) => {
			callback(err||error);
		});
	}
}

module.exports = DeviceHandle;
