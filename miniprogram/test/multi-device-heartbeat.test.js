const assert = require('assert');
const { createMqttService } = require('../services/mqtt');

function createMockMqtt() {
  const events = {};
  const client = {
    on(name, listener) {
      events[name] = events[name] || [];
      events[name].push(listener);
    },
    subscribe(topic, callback) {
      if (callback) callback(null, [{ topic, qos: 0 }]);
    },
    unsubscribe(topic, callback) {
      if (callback) callback(null);
    },
    publish(topic, payload, options, callback) {
      if (callback) callback(null);
    },
    removeAllListeners() {},
    end() {}
  };
  return {
    connect() { return client; },
    emit(name, ...args) {
      (events[name] || []).forEach((listener) => listener(...args));
    }
  };
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

(async () => {
  const mock = createMockMqtt();
  const mqtt = createMqttService({
    mqtt: mock,
    config: { url: 'wxs://test.invalid/mqtt', heartbeatTimeoutMs: 25 }
  });
  mqtt.registerDevice({ id: 'fan-heartbeat', name: '心跳设备' });
  mqtt.connect();
  mock.emit('connect');

  const topic = 'shuangshou/availability/FAN-HEARTBEAT';
  mock.emit('message', topic, 'ONLINE', { retain: true });
  let state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, null);
  assert.strictEqual(state.availability, 'unknown');
  await wait(50);
  state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, null);
  assert.strictEqual(state.availability, 'unknown');

  mock.emit('message', topic, 'ONLINE', { retain: false });
  state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, true);
  assert.strictEqual(state.availability, 'online');
  await wait(50);
  state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, null);
  assert.strictEqual(state.availability, 'unknown');
  assert.match(state.message, /心跳超时/);

  mock.emit('message', topic, 'ONLINE', { retain: false });
  mock.emit('message', 'shuangshou/status/FAN-HEARTBEAT', 'ON', { retain: true });
  await wait(50);
  state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, null);
  assert.strictEqual(state.availability, 'unknown');

  mock.emit('message', topic, 'ONLINE', { retain: false });
  mock.emit('message', topic, 'OFFLINE', { retain: false });
  await wait(50);
  state = mqtt.getDeviceStatus('fan-heartbeat');
  assert.strictEqual(state.online, false);
  assert.strictEqual(state.availability, 'offline');

  mqtt.disconnect();
  console.log('multi-device heartbeat tests passed');
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
