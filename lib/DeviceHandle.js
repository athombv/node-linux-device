var DeviceHandle = require('bindings')('DeviceHandle').DeviceHandle;
var fs = require('fs');
var sysfs_gpio = '/sys/class/gpio';

DeviceHandle.prototype.gpio = function(gpio, value, cb) {
    cb = cb || function(){};
    
    fs.writeFile( sysfs_gpio+'/export', ''+gpio, function(err) {
        fs.writeFile(sysfs_gpio+'/gpio'+gpio+'/direction', 'out', function(err) {
            if(err) return cb(err);
            fs.writeFile(sysfs_gpio+'/gpio'+gpio+'/value', (value ? '1': '0'), function(err) {
                cb(err);
            });
        });
    });
};

exports = module.exports = DeviceHandle;