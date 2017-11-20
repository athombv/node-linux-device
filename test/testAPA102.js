"use strict";

const DH = require('..');

let output = new DH({
    path: '/dev/apa102-0', 
    mode: DH.constants.O_RDWR
});

output.on('data', data => console.log('num leds:', data.toString('utf8').trim()));

(async function test() {
    await output.open();
    output.write(new Buffer([0x05, 0x00, 0x01, 0xff, 0x00, 0xaa, 0x99]));
})();

setTimeout(() => output.close(), 5000);