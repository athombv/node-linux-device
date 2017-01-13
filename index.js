"use strict";

if(process.env.DEVICE_DEBUG_SERVER) {
    let DeviceHandle = require('./remote/client')(process.env.DEVICE_DEBUG_SERVER);
    exports = module.exports = DeviceHandle;
} else {
    let DeviceHandle = require('./lib/DeviceHandle');
    exports = module.exports = DeviceHandle;
}