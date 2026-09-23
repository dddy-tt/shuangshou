const assert = require('assert');
const { createDeviceStore } = require('../services/device-store');
const { createMqttService, parsePayload } = require('../services/mqtt');
const { createRelayGestureGate, resolveGestureTarget } = require('../services/relay-gesture');

function memoryStorage(initial) {
  let value = initial;
  return {
    read: () => value,
    write: (next) => { value = next; },
    value: () => value
  };
}

function createMockMqtt() {
  const events = {};
  const subscriptions = [];
  const unsubscriptions = [];
  const publications = [];
  let clientOptions = null;
  const client = {
    on(name, listener) {
      events[name] = events[name] || [];
      events[name].push(listener);
    },
    subscribe(topic, callback) {
      subscriptions.push(topic);
      if (callback) callback(null, [{ topic, qos: 0 }]);
    },
    unsubscribe(topic, callback) {
      unsubscriptions.push(topic);
      if (callback) callback(null);
    },
    publish(topic, payload, options, callback) {
      publications.push({ topic, payload, options });
      if (callback) callback(null);
    },
    removeAllListeners() {},
    end() {}
  };
  return {
    subscriptions,
    unsubscriptions,
    publications,
    connect(url, options) {
      clientOptions = { url, options };
      return client;
    },
    emit(name, ...args) {
      (events[name] || []).forEach((listener) => listener(...args));
    },
    getClientOptions() { return clientOptions; }
  };
}

function relayState() {
  return {
    connected: true,
    lastFrameAt: 100,
    lastFlexAt: 100,
    flex: Array(10).fill(20),
    fingerStates: Array(10).fill('straight'),
    pose: { roll: 0, pitch: 0, yaw: 0 },
    calibration: { enabledFingers: Array(10).fill(true) }
  };
}

const deviceStorage = memoryStorage(undefined);
const selectionStorage = memoryStorage(undefined);
const bindingStorage = memoryStorage(undefined);
const store = createDeviceStore(deviceStorage, selectionStorage, bindingStorage);
assert.deepStrictEqual(store.list().map((device) => device.id), ['light']);
const fan = store.add({ id: 'fan-A1', name: '风扇' });
const socket = store.add({ id: 'socket-A1', name: '插座' });
assert.strictEqual(fan.id, 'FAN-A1');
assert.strictEqual(socket.id, 'SOCKET-A1');
assert.strictEqual(fan.commandTopic, 'shuangshou/control/FAN-A1');
assert.strictEqual(fan.stateTopic, 'shuangshou/status/FAN-A1');
assert.strictEqual(fan.availabilityTopic, 'shuangshou/availability/FAN-A1');
assert.throws(() => store.add({ id: 'FAN-a1', name: '重复设备' }), /设备 ID 已存在/);
store.select(fan.id);
assert.strictEqual(store.selectedId(), fan.id);
store.bindGesture('gesture-on', fan.id, 'ON');
assert.deepStrictEqual(store.getGestureBinding('gesture-on').deviceId, fan.id);
store.update(fan.id, { id: 'fan-renamed', name: '风扇新名' });
assert.strictEqual(store.getGestureBinding('gesture-on').deviceId, 'FAN-RENAMED');
assert.strictEqual(store.selectedId(), 'FAN-RENAMED');

const deletedDevice = store.add({ id: 'delete-me', name: '待删除设备' });
store.bindGesture('gesture-delete', deletedDevice.id, 'ON');
store.select(deletedDevice.id);
store.remove(deletedDevice.id);
const deletedBinding = store.getGestureBinding('gesture-delete');
assert.strictEqual(deletedBinding.disabled, true);
assert.strictEqual(deletedBinding.mode, 'disabled');
assert.strictEqual(deletedBinding.reason, 'device-removed');
assert.strictEqual(
  resolveGestureTarget(
    { id: 'gesture-delete', action: 'ON' },
    { selectedDeviceId: socket.id, bindings: store.listBindings() }
  ).valid,
  false
);
store.followSelectedGesture('gesture-follow', 'ON');
assert.strictEqual(
  resolveGestureTarget(
    { id: 'gesture-follow' },
    { selectedDeviceId: socket.id, bindings: store.listBindings() }
  ).deviceId,
  socket.id
);
assert.strictEqual(
  resolveGestureTarget({ id: 'legacy-unbound', action: 'ON' }, { selectedDeviceId: socket.id }).deviceId,
  'light'
);
store.remove(socket.id);
assert.strictEqual(store.get(socket.id), null);
const restoredStore = createDeviceStore(deviceStorage, selectionStorage, bindingStorage);
assert.deepStrictEqual(restoredStore.list().map((device) => device.id), ['light', 'FAN-RENAMED']);
assert.strictEqual(restoredStore.selectedId(), 'light');
assert.strictEqual(restoredStore.getGestureBinding('gesture-delete').disabled, true);
assert.strictEqual(restoredStore.getGestureBinding('gesture-follow').mode, 'follow-selected');

const mock = createMockMqtt();
const mqtt = createMqttService({
  mqtt: mock,
  config: { url: 'wxs://test.invalid/mqtt', pendingTimeoutMs: 1000 }
});
mqtt.registerDevices(store.list());
mqtt.connect();
mock.emit('connect');
assert.ok(mock.subscriptions.includes('shuangshou/status/light'));
assert.ok(mock.subscriptions.includes('shuangshou/status/FAN-RENAMED'));
assert.ok(mock.subscriptions.includes('shuangshou/availability/FAN-RENAMED'));
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').online, null);
assert.strictEqual(mqtt.publish('light', 'on'), true);
assert.strictEqual(mqtt.publish('fan-renamed', 'off'), true);
assert.strictEqual(mqtt.publish('fan-renamed', 'on'), false);
assert.deepStrictEqual(mock.publications.slice(-2).map((item) => [item.topic, item.payload]), [
  ['shuangshou/control/light', 'ON'],
  ['shuangshou/control/FAN-RENAMED', 'OFF']
]);
assert.strictEqual(mqtt.getDeviceStatus('light').pending, 'ON');
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').pending, 'OFF');
mock.emit('message', 'shuangshou/status/light', 'ON', { retain: false });
assert.strictEqual(mqtt.getDeviceStatus('light').pending, null);
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').pending, 'OFF');
assert.strictEqual(mqtt.getDeviceStatus('light').availability, 'online');
mock.emit('message', 'shuangshou/status/FAN-RENAMED', 'OFF', { retain: true });
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').pending, 'OFF');
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').historical, true);
mock.emit('message', 'shuangshou/availability/FAN-RENAMED', 'ONLINE', { retain: true });
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').online, null);
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').availability, 'unknown');
mock.emit('message', 'shuangshou/availability/FAN-RENAMED', 'ONLINE', { retain: false });
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').online, true);

const caseMock = createMockMqtt();
const caseMqtt = createMqttService({ mqtt: caseMock, config: { url: 'wxs://test.invalid/mqtt' } });
caseMqtt.registerDevice({ id: 'fan-case', name: '大小写设备' });
caseMqtt.connect();
caseMock.emit('connect');
caseMqtt.registerDevice({ id: 'FAN-CASE', name: '大小写设备' });
assert.ok(caseMock.subscriptions.includes('shuangshou/status/FAN-CASE'));
assert.ok(caseMock.subscriptions.includes('shuangshou/availability/FAN-CASE'));
assert.ok(!caseMock.subscriptions.includes('shuangshou/status/fan-case'));
assert.ok(!caseMock.subscriptions.includes('shuangshou/availability/fan-case'));
caseMqtt.registerDevice({
  id: 'light',
  name: 'legacy 灯新 descriptor',
  commandTopic: 'legacy/demo/control/light',
  stateTopic: 'legacy/demo/status/light',
  availabilityTopic: 'legacy/demo/availability/light'
});
assert.ok(caseMock.unsubscriptions.some((topics) => Array.isArray(topics)
  && topics.includes('shuangshou/status/light')));
assert.ok(caseMock.subscriptions.includes('legacy/demo/status/light'));
caseMock.emit('message', 'shuangshou/status/light', 'ON', { retain: false });
assert.strictEqual(caseMqtt.getDeviceStatus('light').stateKnown, false);
caseMock.emit('message', 'legacy/demo/status/light', 'ON', { retain: false });
assert.strictEqual(caseMqtt.getDeviceStatus('light').stateKnown, true);
caseMqtt.disconnect();

assert.deepStrictEqual(parsePayload('{"state":"OFF"}').state, 'OFF');
assert.strictEqual(parsePayload('not-a-state'), null);
mqtt.disconnect();
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').online, null);
assert.strictEqual(mqtt.getDeviceStatus('fan-renamed').pending, null);

const state = relayState();
const onGesture = { id: 'on', name: '打开', category: 'control', action: 'ON', fingers: Array(10).fill(20), enabled: true };
const offGesture = { id: 'off', name: '关闭', category: 'control', action: 'OFF', fingers: Array(10).fill(20), enabled: true };
const gate = createRelayGestureGate();
const followBinding = { gestureId: 'on', mode: 'follow-selected', action: 'ON' };
let result = gate.update([onGesture], state, {
  selectedDeviceId: 'fan-renamed',
  bindings: [followBinding],
  now: 100
});
assert.strictEqual(result.action, undefined);
result = gate.update([onGesture], state, {
  selectedDeviceId: 'fan-renamed',
  bindings: [followBinding],
  now: 900
});
assert.deepStrictEqual({ deviceId: result.deviceId, action: result.action }, { deviceId: 'FAN-RENAMED', action: 'ON' });
result = gate.update([onGesture], state, {
  selectedDeviceId: 'fan-renamed',
  bindings: [followBinding],
  now: 1000
});
assert.match(result.message, /再次操作/);
gate.reset();
result = gate.update([onGesture, { ...onGesture, id: 'socket-on', deviceId: 'socket-A1' }], state, {
  selectedDeviceId: 'fan-renamed',
  now: 100
});
assert.match(result.message, /一个手势只能控制一个设备/);
assert.strictEqual(result.action, undefined);
assert.deepStrictEqual(resolveGestureTarget({ action: 'LIGHT_ON' }, { selectedDeviceId: 'light' }).action, 'ON');
const strictGate = createRelayGestureGate();
const strictState = relayState();
strictGate.update([onGesture], strictState, {
  selectedDeviceId: fan.id,
  now: 100,
  requireLastFlexAt: true
});
strictState.lastFrameAt = 1800;
const staleFlex = strictGate.update([onGesture], strictState, {
  selectedDeviceId: fan.id,
  now: 1800,
  requireLastFlexAt: true
});
assert.strictEqual(staleFlex.action, undefined);
assert.match(staleFlex.message, /有效的手套数据/);
const missingFlexTimestamp = { ...strictState, lastFlexAt: 0, lastFrameAt: 1800 };
const missingFlexResult = strictGate.update([onGesture], missingFlexTimestamp, {
  selectedDeviceId: fan.id,
  now: 1800,
  requireLastFlexAt: true
});
assert.strictEqual(missingFlexResult.action, undefined);
assert.match(missingFlexResult.message, /lastFlexAt/);
const legacyGate = createRelayGestureGate();
assert.deepStrictEqual(legacyGate.update([onGesture], state, 100), { message: '已识别 ON，请保持 0.8 秒' });
assert.deepStrictEqual(legacyGate.update([onGesture], state, 900), { action: 'ON', message: '触发 ON，请松开手势' });
assert.deepStrictEqual(legacyGate.update([onGesture], state, 1000), { message: '请松开手势后再次操作' });

console.log('multi-device tests passed');
