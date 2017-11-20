"use strict";

const DH = require('./RemoteWrap.js');

//Strips the callback argument, and calls the function without,
//then invokes the callback when the promise returns
function utilCallbackAfterPromise(self, func, args, cb) {
	args = Array.prototype.slice.apply(args);
	let lastarg = args.pop();
	if(!cb) cb = lastarg;
	func.apply(self, args).then(res => {
		try {
			cb(null, res);
		} catch(e) {
			process.nextTick(() => { throw e });
		}
	}).catch((err) => {
		try {
			cb(err);
		} catch(e) {
			process.nextTick(() => { throw e });
		}
	});
}


class DeviceHandle extends DH {
    constructor(opts, opts2) {
        if(typeof opts === 'string') {
            opts = Object.assign({}, opts2||{}, {path: opts});
        }
        super(opts);
    }
    open(cb) {
        if(cb) return utilCallbackAfterPromise(this, super.open, arguments);
        return super.open();
    }
    close(cb) {
        if(cb) return utilCallbackAfterPromise(this, super.close, arguments);
        return super.close();
    }
    flush(cb) {
        if(cb) return utilCallbackAfterPromise(this, super.flush, arguments);
        return super.flush();
    }
    gpio(gpio, value, cb) {
        if(cb) return utilCallbackAfterPromise(this, super.gpio, arguments);
        return super.gpio(gpio, value);
    }
    static gpio(gpio, value, cb) {
        if(cb) return utilCallbackAfterPromise(this, super.gpio, arguments);
        return super.gpio(gpio, value);
    }
}

module.exports = DeviceHandle;