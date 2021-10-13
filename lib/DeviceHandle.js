'use strict';

const { Duplex } = require('stream');
const { EventEmitter } = require('events');
const util = require('util');
const _fs = require('fs');
const tty = require('tty');
const net = require('net');

const FH = require('bindings')('DeviceHandle');

const LDUtils = require('./Utils');

const { Pipe } = process.binding('pipe_wrap');

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
  'BIT_LE': 0x001,
  'BIT_DTR': 0x002,
  'BIT_RTS': 0x004,
  'BIT_ST': 0x008,
  'BIT_SR': 0x010,
  'BIT_CTS': 0x020,
  'BIT_CAR': 0x040,
  'BIT_RNG': 0x080,
  'BIT_DSR': 0x100,
  'BIT_CD': 0x040,
  'BIT_RI': 0x080,
  'BIT_OUT1': 0x2000,
  'BIT_OUT2': 0x4000,
  'BIT_LOOP': 0x8000,
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
    if (typeof options === 'string') options = { path: options };
    const mode = options.mode || CONSTANTS.O_RDWR;
    this[kSource] = {
      path: options.path,
      mode: mode,
      readable: mode & CONSTANTS.O_RDONLY || mode & CONSTANTS.O_RDWR,
      writable: mode & CONSTANTS.O_WRONLY || mode & CONSTANTS.O_RDWR,
      parser: options.parser || undefined,
      reading: 0,
      forcedDataSize: options.absoluteSize,
    };
    if (this[kSource].readable) this[kSource].mode |= CONSTANTS.O_NONBLOCK;
    if (options && options.autoOpen) this.open().catch(err => this.emit('error', err));
  }

  __onEnd(err) {
    if (err) this.close().catch(e => {
    });
  }

  __onError(err) {
    if (err) this.close().catch(e => {
    });
    this.emit('error', err);
  }

  __pushSmart(res) {
    let result = true;
    if (res && this[kSource].parser) {
      if (!this[kSource].emitter) {
        this[kSource].emitter = new EventEmitter();
        this[kSource].emitter.on('data', (data) => this.push(data));
      }
      this[kSource].parser(this[kSource].emitter, res);
    }
    if (res && this[kSource].forcedDataSize) {
      for (let i = 0; i < res.length; i += this[kSource].forcedDataSize) {
        result = this.push(res.slice(i, i + this[kSource].forcedDataSize));
      }
    } else {
      result = this.push(res);
    }
    return result;
  }

  async _createStreams() {
    if (tty.isatty(this.fd)) {
      this[kSource].isTTY = true;
      if (this[kSource].readable) {
        this[kSource].inStream = new tty.ReadStream(this.fd);
      }
      //write uses writeRepeated instead of a stream
    } else {
      this[kSource].isTTY = false;
      const fstat = await fs.fstat(this.fd);

      if (fstat.isCharacterDevice() && this[kSource].mode & CONSTANTS.O_NONBLOCK) {
        let handle = new Pipe(0);
        handle.open(this.fd);
        this[kSource].inStream = new net.Socket({
          handle: handle,
          readable: this[kSource].readable,
          writable: false,
        });
      } else if (fstat.isFile() || fstat.isCharacterDevice() || fstat.isBlockDevice()) {
        //do nothing
        this[kSource].useFSRead = true;
      } else if (fstat.isSocket()) {
        this[kSource].inStream = new net.Socket({
          fd: this.fd,
          readable: this[kSource].readable,
          writable: false,
        });
      } else {
        throw new Error('unknown_type');
      }
    }

    if (this[kSource].inStream) {
      this[kSource].inStream.on('error', err => this.__onError(err));
      this[kSource].inStream.on('end', err => this.__onEnd(err));
      this[kSource].inStream.pause();
      this[kSource].inStream.on('data', (data) => {
        if (!this.__pushSmart(data)) {
          this[kSource].inStream.pause();
        }
      });
      if (this[kSource].resumeOnCreate) {
        delete this[kSource].resumeOnCreate;
        this[kSource].inStream.resume();
      }
    } else {
      if (this[kSource].resumeOnCreate) {
        delete this[kSource].resumeOnCreate;
        this._read();
      }
    }
  }

  /**
   * Opens the device
   * @returns {number} Device fd
   */
  async open() {
    if (this.fd) await this.close().catch(() => {
    });
    if (this[kSource].tainted) throw new Error('attempt to reuse closed DeviceHandle');
    if (!this[kSource].opening) {
      return this[kSource].opening = (async () => {
        const res = await fs.open(this[kSource].path, this[kSource].mode);
        this.fd = res;
        this[kSource].opening = false;
        await this._createStreams();
        this.emit('open', res);
        return res;
      })();
    } else {
      return this[kSource].opening;
    }
  }

  /**
   * Closes the device
   */
  async close() {
    if (this[kSource].outStream) {
      this[kSource].outStream.cork();
      this[kSource].outStream.removeAllListeners();
      delete this[kSource].outStream;
    }
    if (this[kSource].inStream) {
      this[kSource].inStream.removeAllListeners();
      this[kSource].inStream.on('error', () => {
      });
      this[kSource].inStream.destroy();
      delete this[kSource].inStream;
    }
    if (!this.fd) return;
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
    if (!this.fd) throw new Error('not_open');
    await FH.ioctl(this.fd, direction, type, cmd, data || Buffer.alloc(0));
  }

  /**
   * Performs a raw ioctl on the device
   * @param {number} cmd - The ioctl command
   * @param {Buffer} data - The ioctl data, the data may be changed by the ioctl
   */
  async ioctlRaw(cmd, data) {
    if (!this.fd) throw new Error('not_open');
    await FH.ioctlRaw(this.fd, cmd, data || Buffer.alloc(0));
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
    await fs.writeFile(SYSFS_GPIO + '/export', '' + gpio).catch(e => {
    });
    await fs.writeFile(SYSFS_GPIO + '/gpio' + gpio + '/direction', value ? 'high' : 'low');
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

  _allocNewPool(poolSize) {
    this[kSource].pool = Buffer.allocUnsafe(poolSize);
    this[kSource].pool.used = 0;
  }

  _readFS(size) {
    // the actual read.

    if (!this[kSource].pool || this[kSource].pool.length - this[kSource].pool.used < 512) {
      // discard the old pool.
      this._allocNewPool(2048);
    }

    // Grab another reference to the pool in the case that while we're
    // in the thread pool another read() finishes up the pool, and
    // allocates a new one.
    var thisPool = this[kSource].pool;
    var toRead = Math.min(thisPool.length - thisPool.used, size || 512);
    var start = thisPool.used;

    _fs.read(this.fd, thisPool, start, toRead, null, (err, bytesRead) => {
      if (err) {
        return this.__onError(err);
      }

      if (bytesRead > 0) {
        let buffer = thisPool.slice(start, start + bytesRead);
        if (thisPool.used == start + toRead) thisPool.used = start + bytesRead;
        this.__pushSmart(buffer);
      } else {
        //setTimeout(() => this._readFS(), 1000);
      }
    });
    thisPool.used += toRead;
  }

  _read(size) {
    if (this[kSource].inStream && this[kSource].inStream.isPaused()) {
      this[kSource].inStream.resume();
    } else if (this[kSource].useFSRead) {
      this._readFS(size);
    } else {
      this[kSource].resumeOnCreate = true;
    }
  }

  async _writePromise(chunk, encoding) {
    if (!this.fd) throw new Error('not_open');
    if (!this[kSource].writable) throw new Error('not_writable');

    // Check if a custom header is available which specifies the interval and repetitions of this command. Encoding happens in node-homey-infrared/index.js.
    const { buffer, interval, repetitions } = LDUtils.decodeWriteBuffer(chunk);

    return await FH.writeRepeated(this.fd, buffer, interval, repetitions); //retain chunks
  }

  _destroy(error, callback) {
    if (!this.fd) return;
    util.callbackify(this.close).call(this, (err) => {
      callback(err || error);
    });
  }
}

module.exports = DeviceHandle;
