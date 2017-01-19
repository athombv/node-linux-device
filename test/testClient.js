"use strict";
process.env.DEVICE_DEBUG_SERVER = 'ws://localhost:8081';
const DH = require('..');
let input = new DH('/dev/input/by-path/platform-gpio_keys-event',false,16,console.log);
DH.gpio(58, 0, console.log.bind(0,'gpio cb'));

setTimeout(() => {
    input.write(new Buffer([0xFF, 0xFA, 0xA0, 0x00]), (err, res) => console.log(err, res));
}, 5000);
