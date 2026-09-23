const assert = require('assert');
const { createAlarmAckController } = require('../services/alarm-ack');

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function main() {
  let currentDevice = 'device-a';
  let connectionToken = 1;
  const writes = [];
  const stages = [];
  const controller = createAlarmAckController({
    timeoutMs: 12,
    maxAttempts: 3,
    write: (command) => { writes.push(command); return Promise.resolve(); },
    getCurrentDeviceId: () => currentDevice,
    getConnectionToken: () => connectionToken,
    onAttempt: () => stages.push('sending'),
    onWaiting: () => stages.push('waiting'),
    onResult: (result) => stages.push(result.ok ? 'resolved' : result.reason)
  });

  const resolvedRequest = controller.request({ deviceId: 'device-a', boot: 7, id: 9, alarmType: 1, active: true });
  const duplicateRequest = controller.request({ deviceId: 'device-a', boot: 7, id: 9, alarmType: 1, active: true });
  assert.strictEqual(resolvedRequest, duplicateRequest, '同一当前事件的 ACK 请求应 single-flight');
  await delay(1);
  assert.deepStrictEqual(writes, ['ALARM_ACK:7:9\n']);
  assert.strictEqual(controller.notifyResolved({ deviceId: 'device-a', boot: 7, id: 9, alarmType: 2, active: false }), false, '不同 TYPE 不能完成 ACK');
  assert.strictEqual(controller.notifyResolved({ deviceId: 'device-a', boot: 7, id: 9, alarmType: 1, active: false }), true);
  const resolved = await resolvedRequest;
  assert.strictEqual(resolved.ok, true);
  assert.strictEqual(resolved.attempts, 1);
  assert.deepStrictEqual(stages, ['sending', 'waiting', 'resolved']);

  writes.length = 0;
  const timeoutResult = await controller.request({ deviceId: 'device-a', boot: 7, id: 10, alarmType: 2, active: true });
  assert.strictEqual(timeoutResult.ok, false);
  assert.strictEqual(timeoutResult.reason, 'active-zero-timeout');
  assert.strictEqual(writes.length, 3, 'ACK 超时应有界重试');
  assert.ok(writes.every((command) => command === 'ALARM_ACK:7:10\n'), '重试必须使用同一 BOOT/ID');

  writes.length = 0;
  const staleRequest = controller.request({ deviceId: 'device-a', boot: 7, id: 11, alarmType: 1, active: true });
  await delay(1);
  currentDevice = 'device-b';
  connectionToken = 2;
  const stale = await staleRequest;
  assert.strictEqual(stale.reason, 'stale-device');
  assert.strictEqual(controller.notifyResolved({ deviceId: 'device-a', boot: 7, id: 11, active: false }), false, '旧设备回帧不能完成新连接的 ACK');

  controller.cancel();

  let hangingWrites = 0;
  const writeTimeout = createAlarmAckController({
    timeoutMs: 5,
    maxAttempts: 2,
    write: () => { hangingWrites += 1; return new Promise(() => {}); },
    getCurrentDeviceId: () => 'device-c'
  });
  const writeTimeoutResult = await writeTimeout.request({ deviceId: 'device-c', boot: 1, id: 1, alarmType: 1, active: true });
  assert.strictEqual(writeTimeoutResult.reason, 'write-failed', 'BLE 写入挂起也必须受到 ACK 超时边界约束');
  assert.strictEqual(hangingWrites, 2);

  console.log('alarm ack tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
