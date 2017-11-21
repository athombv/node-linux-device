"use strict";

const {Duplex} = require('stream');
const {EventEmitter} = require('events');
const util = require('util');
const fs = require('fs');
const FH = require('bindings')('DeviceHandle');

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
		const mode = options.mode || CONSTANTS.O_RDWR;
		this[kSource] = {
			path: options.path,
			mode: mode,
			readable: mode & CONSTANTS.O_RDONLY || mode & CONSTANTS.O_RDWR,
			writable: mode & CONSTANTS.O_WRONLY || mode & CONSTANTS.O_RDWR,
			parser: options.parser || undefined,
			handle: new FH(options.path),
			reading: 0,
			forcedDataSize: options.absoluteSize,
		}
		if(options && options.autoOpen) this.open().catch(err => this.emit('error', err));
	}

	/**
	 * Opens the device
	 * @returns {number} Device fd
	 */
	async open() {
		if(this.fd) await this.close().catch(()=>{});
		const res = await this[kSource].handle.open(this[kSource].mode | CONSTANTS.O_DSYNC);
		this.fd = res;
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
		if(!this.fd) return;
		await this[kSource].handle.close();
		this.push(null);
		this.emit('close');
		delete this.fd;
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
		await this[kSource].handle.ioctl(direction, type, cmd, data||Buffer.alloc(0));
	}
	
	/**
	 * Performs a raw ioctl on the device
	 * @param {number} cmd - The ioctl command
	 * @param {Buffer} data - The ioctl data, the data may be changed by the ioctl
	 */
	async ioctlRaw(cmd, data) {
		if(!this.fd) throw new Error('not_open');
		await this[kSource].handle.ioctlRaw(cmd, data||Buffer.alloc(0));
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
		return new Promise((resolve, reject) => {
			fs.writeFile( SYSFS_GPIO+'/export', ''+gpio, function(err) {
				fs.writeFile(SYSFS_GPIO+'/gpio'+gpio+'/direction', value ? 'high': 'low', function(err) {
					if(err) return reject(err);
					resolve(err);
				});
			});
		});
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

	async _writePromise(chunk, encoding) {
		if(!this.fd) throw new Error('not_open');
		if(!this[kSource].writable) throw new Error('not_writable');
		if(chunk.interval && chunk.repetitions) {
			return await this[kSource].handle.writeRepeated(chunk, chunk.interval, chunk.repetitions); //retain chunks
		} else {
			return await this[kSource].handle.write(chunk); //retain chunk
		}
	}

	_read() {
		if(!this[kSource].readable) return;
		this[kSource].reading += 1;
		if(!this.fd) return;
		this[kSource].handle.read((err, res) => {

			if(err) {
				return process.nextTick(() => {
					this.destroy(err);
				});
			}
			
			if(res && this[kSource].parser) {
				if(!this[kSource].emitter) {
					this[kSource].emitter = new EventEmitter();
					this[kSource].emitter.on('data', (data) => this.push(data));
				}
				this[kSource].parser(this[kSource].emitter, res);
			} else if(res && this[kSource].forcedDataSize) {
				for(let i = 0; i < res.length; i+= this[kSource].forcedDataSize) {
					this.push(res.slice(i, i+this[kSource].forcedDataSize));
				}
			} else {
				this.push(res);
			}

			if(!res) return this[kSource].handle.stopReading();
	
			this[kSource].reading -= 1;
			
			if(this[kSource].reading < 0) {
				this[kSource].reading = 0;
				this[kSource].handle.stopReading();
			}
		});
	}
	
	_destroy(error, callback) {
		if(!this.fd) return;
		util.callbackify(this.close).call(this, (err) => {
			callback(err||error);
		});
	}
}

module.exports = DeviceHandle;