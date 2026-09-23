const assert = require('assert');
const { resolveGestureTarget } = require('../services/relay-gesture');

let definition;
global.Page = (value) => { definition = value; };

const runtimeState = {
  connected: false,
  flex: Array(10).fill(null),
  calibration: { enabledFingers: Array(10).fill(true) },
  statusText: '蓝牙未连接',
  safetyAlert: null,
  buzzerStatus: '',
  lastFrameAt: 0,
  lastFlexAt: 0
};
const runtime = {
  modes: { remote: 'remote' },
  subscribe(listener) { listener(runtimeState); return () => {}; },
  getState() { return runtimeState; },
  setMode() {},
  acknowledgeSafety() {}
};
global.getApp = () => ({ getRuntime: () => runtime });
require('../pages/remote/remote');

const page = {
  ...definition,
  data: JSON.parse(JSON.stringify(definition.data)),
  setData(patch) { Object.assign(this.data, patch); }
};
page.onLoad();
page.onShow();
assert.deepStrictEqual(page.data.devices.map((device) => device.id), ['light']);
assert.strictEqual(page.data.devices[0].availabilityText, '设备状态未知');

page.openAddDevice();
page.setData({ deviceNameDraft: '风扇', deviceIdDraft: 'fan-page' });
page.saveDevice();
assert.strictEqual(page.data.selectedDeviceId, 'FAN-PAGE');
assert.ok(page.data.devices.some((device) => device.name === '风扇'));

page.openEditDevice({ currentTarget: { dataset: { deviceId: 'FAN-PAGE' } } });
page.setData({ deviceNameDraft: '客厅风扇', deviceIdDraft: 'fan-page' });
page.saveDevice();
assert.ok(page.data.devices.some((device) => device.name === '客厅风扇'));

const gesture = {
  id: 'page-delete-gesture',
  name: '页面删除回归',
  category: 'control',
  action: 'ON',
  fingers: Array(10).fill(20),
  enabled: true
};
page.gestureStore.add(gesture);
const socket = page.deviceStore.add({ id: 'socket-page', name: '插座' });
page.deviceStore.bindGesture(gesture.id, 'FAN-PAGE', 'ON');
page.deviceStore.select(socket.id);
page.refreshDevices();
assert.strictEqual(
  page.data.controlGestures.find((item) => item.id === gesture.id).bindingDeviceId,
  'FAN-PAGE'
);

page.handleSelectDevice({ currentTarget: { dataset: { deviceId: 'light' } } });
assert.strictEqual(page.data.selectedDeviceId, 'light');
page.deviceStore.select(socket.id);
page.refreshDevices();
page.handleDeleteDevice({ currentTarget: { dataset: { deviceId: 'FAN-PAGE' } } });
assert.ok(!page.data.devices.some((device) => device.id === 'FAN-PAGE'));
const deletedBinding = page.deviceStore.getGestureBinding(gesture.id);
assert.strictEqual(deletedBinding.disabled, true);
assert.strictEqual(deletedBinding.mode, 'disabled');
assert.strictEqual(
  resolveGestureTarget(
    { id: gesture.id },
    { selectedDeviceId: socket.id, bindings: page.deviceStore.listBindings() }
  ).valid,
  false
);
assert.strictEqual(
  page.data.controlGestures.find((item) => item.id === gesture.id).bindingName,
  '已禁用：原设备已删除'
);

// 只有用户显式选择第二个选项，失效绑定才会切换到 follow-selected。
page.handleGestureBindingChange({
  currentTarget: { dataset: { gestureId: gesture.id } },
  detail: { value: 1 }
});
assert.strictEqual(page.deviceStore.getGestureBinding(gesture.id).mode, 'follow-selected');
assert.strictEqual(
  resolveGestureTarget(
    { id: gesture.id },
    { selectedDeviceId: socket.id, bindings: page.deviceStore.listBindings() }
  ).deviceId,
  socket.id
);
page.onUnload();

console.log('multi-device page smoke test passed');
