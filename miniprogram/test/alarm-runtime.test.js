const assert = require('assert');
const { createAlarmRuntime } = require('../services/alarm-runtime');

let now = 1000;
const saved = [];
let attentionCount = 0;
let resolvedCount = 0;
const runtime = createAlarmRuntime({
  clock: () => now,
  storage: { read: () => saved.slice(), write: (key, value) => { saved.splice(0, saved.length, ...value); } },
  onAttention: () => { attentionCount += 1; },
  onResolved: () => { resolvedCount += 1; }
});

const first = runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: true }, { source: 'ble', deviceId: 'device-a' });
assert.strictEqual(first.accepted, true);
assert.strictEqual(runtime.getState().active.length, 1);
assert.strictEqual(attentionCount, 1);

const duplicate = runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: true }, { source: 'guardian', deviceId: 'device-a', deviceConfirmed: true });
assert.strictEqual(duplicate.reason, 'duplicate-active');
assert.strictEqual(attentionCount, 1, '同一 device/BOOT/ID 不应重复播放报警音');
assert.deepStrictEqual(runtime.find('device-a/11/4').sources.sort(), ['ble', 'guardian']);

const cloudClose = runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: false }, { source: 'guardian', deviceId: 'device-a' });
assert.strictEqual(cloudClose.reason, 'cloud-resolution-not-authoritative');
assert.strictEqual(runtime.getState().active.length, 1, '云端普通回执不能消除固件活动事件');
assert.strictEqual(runtime.ingest({ source: 'guardian', deviceId: 'device-a', boot: 11, id: 4,
  alarmType: 1, active: false, cloudConfirmed: true, deviceConfirmed: true },
{ cloudConfirmed: true }).reason, 'cloud-resolution-not-authoritative', '输入字段不能自行声明认证查询权限');
assert.strictEqual(runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: false }, { source: 'guardian', origin: 'device', deviceId: 'device-a', deviceConfirmed: true }).reason, 'cloud-resolution-not-authoritative');

assert.strictEqual(runtime.acknowledge('device-a/11/4', 'guardian').accepted, true);
assert.strictEqual(runtime.requestResolve('device-a/11/4', 'guardian').reason, 'remote-resolve-forbidden');
assert.strictEqual(runtime.requestResolve('device-a/11/4', 'wearer').accepted, true);

assert.strictEqual(runtime.hideLocally('device-a/11/4').reason, 'acknowledge-first',
  '未由本机确认的活动报警不能仅靠隐藏操作移除');
assert.strictEqual(runtime.acknowledge('device-a/11/4', 'local').accepted, true);
const hidden = runtime.hideLocally('device-a/11/4');
assert.strictEqual(hidden.accepted, true);
assert.strictEqual(runtime.getState().active.length, 0, '仅隐藏后不应继续遮挡当前本机界面');
assert.strictEqual(runtime.getState().allActive.length, 1, '仅隐藏不能把活动设备事件伪造成已解除');
assert.strictEqual(runtime.getState().history[0].locallyHidden, true);
const reappeared = runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: true }, { source: 'ble', deviceId: 'device-a' });
assert.strictEqual(reappeared.action, 'revealed-active', '设备再次确认活动报警后必须重新展示');
assert.strictEqual(runtime.getState().active.length, 1);
assert.strictEqual(runtime.getState().history[0].locallyHidden, false);
assert.strictEqual(attentionCount, 2, '重新展示的真实设备报警应再次触发本地关注回调');

now = 1200;
const close = runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: false }, { source: 'ble', deviceId: 'device-a' });
assert.strictEqual(close.action, 'resolved');
assert.strictEqual(runtime.getState().active.length, 0);
assert.strictEqual(resolvedCount, 1);
assert.strictEqual(runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: true }, { source: 'ble', deviceId: 'device-a' }).reason, 'already-resolved');

assert.strictEqual(runtime.ingest({ type: 'alarm', boot: 11, id: 4, alarmType: 1, active: true }, { source: 'ble', deviceId: 'device-b' }).accepted, true, '设备标识必须参与唯一键');

console.log('alarm runtime tests passed');

const mixed = createAlarmRuntime({ storage: { read: () => [], write() {} }, authorizeCloudSnapshot: () => true });
const mixedEvent = { deviceId: 'device-mixed', boot: 1, id: 1, alarmType: 1, active: true };
mixed.ingest(mixedEvent, { source: 'ble' });
assert.strictEqual(mixed.ingest({ ...mixedEvent, active: false }, { source: 'guardian' }).reason,
  'cloud-resolution-not-authoritative', '即使查询已认证，也不能解除本机 BLE 活动事件');
assert.strictEqual(mixed.getState().active.length, 1);
