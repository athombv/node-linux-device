# linux-device
Native addon to communicate with linux devices (can also be used for sockets or FIFOs). 

### Installation

Install with `npm`:

``` bash
$ npm install linux-device
```

### Usage
See the <a href="http://athombv.github.io/node-linux-device/DeviceHandle.html">API Docs</a> for more information.

### Remote usage
It is possible to use this module to access devices remotely. In order to do this, run the `remote-device-server` binary on your slave device, and export the `DEVICE_DEBUG_SERVER` environment variable to point to the remote end, eg `EXPORT DEVICE_DEBUG_SERVER=ws://remote-device.local:8081` 
