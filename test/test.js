"use strict";

if(process.argv[2] == 'remote') {
    console.log('Testing remote');
    process.env.DEVICE_DEBUG_SERVER = "ws://localhost:8081";

    const server = require('../remote/server')(8081);
}

const DH = require('..');

let input = new DH({
    path: '/dev/zero', 
    mode: DH.constants.O_RDWR,
    absoluteSize: 16
});

let output = new DH({
    path: '/dev/null', 
    mode: DH.constants.O_RDWR
});


input.on('open', console.log.bind(null, 'open'));
input.on('error', console.log.bind(null, 'error'));
input.on('data', console.log.bind(null, 'data'));
input.on('close', console.log.bind(null, 'close'));
input.on('end', console.log.bind(null, 'end'));
output.on('error', console.log.bind(null, 'error'));
output.on('end', console.log.bind(null, 'end'));

(async function test() {
    await input.open();
    await output.open();
    input.pipe(output);
})();