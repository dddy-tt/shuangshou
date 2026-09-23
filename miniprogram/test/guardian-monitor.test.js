const assert = require('assert');
const { createGuardianMonitor } = require('../services/guardian-monitor');

const events = [];
const statuses = [];
const monitor = createGuardianMonitor({
  api: {
    listEvents: () => Promise.resolve({ events: [
      { eventKey: 'device-a/2/1', wearableId: 'device-a', boot: 2, id: 1, alarmType: 2, active: true, origin: 'device', deviceConfirmed: true },
      { eventKey: 'device-a/2/2', wearableId: 'device-a', boot: 2, id: 2, alarmType: 1, active: false, origin: 'device', deviceConfirmed: false }
    ] }),
    getStatus: () => Promise.resolve({ status: { wearableId: 'device-a', online: true, lastSeenAt: Date.now() } })
  },
  onEvent: (event) => events.push(event),
  onStatus: (status) => statuses.push(status),
  pollIntervalMs: 3000,
  offlineAfterMs: 60000
});

return monitor.start().then(() => {
  monitor.stop();
  assert.strictEqual(monitor.getState().running, false, '停止监护时应清理轮询定时器');
  assert.strictEqual(events.length, 2);
  assert.strictEqual(events.find((event) => event.eventKey === 'device-a/2/1').deviceConfirmed, true, '监护轮询必须保留服务端的设备确认快照');
  assert.strictEqual(events.find((event) => event.eventKey === 'device-a/2/2').deviceConfirmed, false, '未被服务端确认的云端快照必须保持未确认');
  assert.strictEqual(statuses[0].online, true);
  let releaseFirst;
  let calls = 0;
  const restarted = createGuardianMonitor({
    api: {
      listEvents: () => {
        calls += 1;
        if (calls === 1) return new Promise((resolve) => { releaseFirst = resolve; });
        return Promise.resolve({ events: [] });
      },
      getStatus: () => Promise.resolve({ status: { wearableId: 'device-a', online: true, lastSeenAt: Date.now() } })
    },
    pollIntervalMs: 3000,
    offlineAfterMs: 60000
  });
  const first = restarted.start();
  return new Promise((resolve) => setImmediate(resolve)).then(() => {
    restarted.stop();
    const second = restarted.start();
    return second.then(() => {
      assert.ok(calls >= 2, 'stop 后立刻 start 必须创建新一轮轮询，不能被旧 inFlight 卡住');
      releaseFirst();
      return first;
    });
  }).then(() => {
    restarted.stop();
    console.log('guardian monitor tests passed');
  });
}).catch((error) => { console.error(error); process.exitCode = 1; });
