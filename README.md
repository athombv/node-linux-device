# node-linux-device
Native addon to communicate with linux devices (can also be used for sockets or FIFOs). 
All read and write operations are done in separate threads, resulting in higher performance compared to NodeJS filesystem functions

Installation
------------

Install with `npm`:

``` bash
$ npm install linux-device
```

API
--------

**new DeviceHandle(String path, Boolean enableWrite, Number objectSize, [Number minimalObjectSize,] Function callback(Error err, Buffer data) )**

Creates a new DeviceHandle instance.

 - `Path` A path to a file or device
 - `enableWrite` True if file should be opened in write mode
 - `objectSize` Maximal size in bytes for each callback event
 - `minimalObjectSize` Minimal size in bytes for each callback event (defaults to ObjectSize)
 - `callback(Error err, Buffer data)` Callback function that is invoked whenever data of `objectSize` is available.

**DeviceHandle.write(Buffer data [, Object opts], Function callback(Error err) )**

Writes data to the device or file.

- `data` A buffer containing the data to write (append)
- `opts` [optional] An object containing write options. Supported values are `repetitions` and `interval`
- `callback(Error err)` Callback function containing an optional error while writing.

**DeviceHandle.ioctl(Number direction, Number type, Number cmd [, Buffer buffer])**

Performs an `ioctl` on the open device. Linux only.

- `direction` Either `DeviceHandle.IOCTL_READ`, `DeviceHandle.IOCTL_WRITE`, `DeviceHandle.IOCTL_RW` or `DeviceHandle.IOCTL_NONE`
- `type` The type of the ioctl
- `cmd` The command number of the ioctl
- `buffer` [optional] A buffer containing in and/or output of the ioctl

**DeviceHandle.ioctl_raw(Number cmd [, Buffer buffer])**

Performs a raw `ioctl` on the open device. Usage is discouraged. Linux only.

- `cmd` The command number of the ioctl, including direction, type and cmd
- `buffer` [optional] A buffer containing in and/or output of the ioctl

**DeviceHandle.close([Function callback()])**

Closes the current device and invoke callback when the device is closed.

- `callback()` [optional] A callback function

Examples
--------


```
var DeviceHandle = require('linux-device');

var LIRC_IOCTL_TYPE = "i".charCodeAt(0);
var LIRC_IOCTL_SET_SEND_CARRIER = 0x13;
var LIRC_INTERVAL_SIZE = 4;

//Open device
var device = new DeviceHandle('/dev/lirc0', true, LIRC_INTERVAL_SIZE, function(err, data) {
	if(err) return console.log("ERROR:", err);
	console.log("received interval:", data.readUInt32LE(0).toString(16));
});

//Sets the IR carrier frequency
function setCarrier(carrier) {
	var buffer = new Buffer(4);
	buffer.writeUInt32LE(carrier, 0);
	try {
		device.ioctl(DeviceHandle.IOCTL_WRITE, 
				LIRC_IOCTL_TYPE, LIRC_IOCTL_SET_SEND_CARRIER, buffer);
	} catch(e) {
		console.log("An error occurred while performing lirc ioctl:", e);
	}
}

//Transmits IR data through LIRC
function writeLircData(data, callback) {
	if(!(data instanceof Buffer)) {
		throw new Error("data must be an instance of Buffer");
	}
	var buffer = new Buffer(data.length*LIRC_INTERVAL_SIZE);
	data.forEach(function(interval, i) {
		buffer.writeUInt32LE(interval, i*LIRC_INTERVAL_SIZE);
	}
	device.write(data, callback);
}

//set the IR carrier frequency
setCarrier(38000);

//Transmit an IR beam every 2 seconds
var interval = setInterval(function() {
	writeLircData([100,200,100,200,100], function() {
		console.log("IR beam transmitted!");
	});
}, 2000);

//close the device after 20 seconds
setTimeout(function() {
	clearInterval(interval);
	console.log("Closing device...");
	device.close(function() {
		console.log("Device closed!");
	});
}, 20000);

```