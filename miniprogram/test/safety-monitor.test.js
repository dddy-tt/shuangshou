const assert = require('assert');
const { createSafetyMonitor } = require('../services/safety-monitor');

const monitor = createSafetyMonitor({
  jyValid: false
});

assert.strictEqual(monitor.push({ roll: 0, pitch: 0, yaw: 0 }), null, '姿态角变化不再触发安全报警');
assert.strictEqual(monitor.push({ roll: 50, pitch: 0, yaw: 0 }), null, '姿态角变化不再触发安全报警');
const alarmWithoutJY = monitor.pushAlarm({ type: 'alarm', boot: 1, id: 2, alarmType: 1, active: true }, { source: 'ble', deviceId: 'ble-1', jyValid: false });
assert.strictEqual(alarmWithoutJY.deviceConfirmed, true, '有效 ALARM 已由固件锁存，不能被当前 JY 诊断状态丢弃');

monitor.setJYStatus({ jy: 1, jyRet: 0 });
const alarm = monitor.pushAlarm({ type: 'alarm', boot: 1, id: 2, alarmType: 1, active: true }, { source: 'ble', deviceId: 'ble-1' });
assert.strictEqual(alarm.deviceConfirmed, true);
assert.strictEqual(alarm.alarmType, 1);

console.log('safety monitor tests passed');
