const assert = require('assert');
const fs = require('fs');

const source = fs.readFileSync('miniprogram/device-firmware/esp01-relay/src/main.cpp', 'utf8');
const platformio = fs.readFileSync('miniprogram/device-firmware/esp01-relay/platformio.ini', 'utf8');
const readme = fs.readFileSync('miniprogram/device-firmware/esp01-relay/README.md', 'utf8');

assert.match(platformio, /board\s*=\s*esp01_1m/);
assert.match(platformio, /PubSubClient/);
assert.match(platformio, /WiFiManager/);
assert.match(source, /ESP\.getChipId\(\)/);
assert.match(source, /body == "ON"/);
assert.match(source, /body == "OFF"/);
assert.match(source, /"OFFLINE"/);
assert.match(source, /publishAvailability\("ONLINE"\)/);
assert.match(source, /HEARTBEAT_INTERVAL_MS/);
assert.match(source, /RELAY_PIN = 0/);
assert.match(source, /RELAY_ACTIVE_LOW = true/);
assert.match(source, /YOUR_MQTT_BROKER_HOST/);
assert.match(readme, /禁止.*真实无人值守/);
assert.match(readme, /packOptions\.ignore/);

console.log('multi-device firmware template tests passed');
