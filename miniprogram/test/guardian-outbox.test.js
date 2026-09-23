const assert = require('assert');
const { createGuardianOutbox } = require('../services/guardian-outbox');

function memoryStorage() {
  let value;
  return {
    read: () => value,
    write: (key, next) => { value = next; },
    value: () => value
  };
}

async function main() {
  let now = 1000;
  let online = false;
  const sent = [];
  const storage = memoryStorage();
  const outbox = createGuardianOutbox({
    storage,
    clock: () => now,
    baseDelayMs: 10,
    maxDelayMs: 40,
    maxAttemptsPerFlush: 10,
    send: (event) => {
      if (!online) return Promise.reject(new Error('offline'));
      sent.push({ eventKey: event.eventKey, active: event.active });
      return Promise.resolve({ ok: true });
    }
  });
  const active = { deviceId: 'device-a', boot: 5, id: 3, alarmType: 1, active: true, origin: 'device', timestamp: 1000 };
  const resolved = { ...active, active: false, timestamp: 1010 };
  assert.strictEqual(outbox.enqueue(active).accepted, true);
  assert.strictEqual(outbox.enqueue(resolved).accepted, true);
  assert.deepStrictEqual(outbox.getState().items.map((item) => item.phase), ['active', 'resolve']);

  const failed = await outbox.flush({ canSend: () => true });
  assert.strictEqual(failed.failed, 1);
  assert.strictEqual(failed.pending, 2, 'ACTIVE 失败时 ACTIVE/RESOLVE 都必须保留');
  assert.strictEqual(sent.length, 0);
  now = 1011;
  online = true;
  const recovered = await outbox.flush({ canSend: (event) => event.deviceId === 'device-a' });
  assert.strictEqual(recovered.ok, true);
  assert.deepStrictEqual(sent, [
    { eventKey: 'device-a/5/3', active: true },
    { eventKey: 'device-a/5/3', active: false }
  ], '恢复后必须按 ACTIVE -> RESOLVE 顺序重试');
  assert.strictEqual(outbox.getState().pending, 0);

  const restored = createGuardianOutbox({ storage, send: () => Promise.resolve() });
  assert.strictEqual(restored.getState().pending, 0, '成功发送后的持久化队列应为空');

  const directStorage = memoryStorage();
  const directSent = [];
  const direct = createGuardianOutbox({
    storage: directStorage,
    baseDelayMs: 0,
    send: (event) => {
      directSent.push({ eventKey: event.eventKey, active: event.active, synthetic: event.synthetic });
      return Promise.resolve({ ok: true });
    }
  });
  const directResolve = { ...active, deviceId: 'device-direct', bindingId: 'binding-direct', active: false };
  assert.strictEqual(direct.enqueue(directResolve).accepted, true);
  assert.deepStrictEqual(direct.getState().items.map((item) => item.phase), ['resolve'], '首次 RESOLVE 不得补造 ACTIVE');
  assert.strictEqual((await direct.flush()).ok, true);
  assert.deepStrictEqual(directSent, [{ eventKey: 'device-direct/5/3', active: false, synthetic: false }], '服务端允许直接创建已解除事件');
  assert.strictEqual(direct.enqueue(directResolve).reason, 'deduplicated', '直接 RESOLVE 成功后必须幂等去重');
  assert.strictEqual(direct.enqueue({ ...directResolve, active: true }).reason, 'deduplicated', '已解除后迟到 ACTIVE 不得重新上报');
  const restoredDirect = createGuardianOutbox({ storage: directStorage });
  assert.strictEqual(restoredDirect.enqueue(directResolve).reason, 'deduplicated', '直接 RESOLVE 的幂等记录必须跨重启保留');
  const legacyStorage = memoryStorage();
  legacyStorage.write('', { items: [
    { ...directResolve, eventKey: 'device-direct/5/3', phase: 'active', synthetic: true, sequence: 1 },
    { ...directResolve, eventKey: 'device-direct/5/3', phase: 'resolve', sequence: 2 }
  ] });
  const legacySent = [];
  const legacy = createGuardianOutbox({ storage: legacyStorage, send: async (event) => { legacySent.push(event.active); } });
  await legacy.flush();
  assert.deepStrictEqual(legacySent, [false], '旧队列恢复不能发送之前补造的 synthetic ACTIVE');

  const tamperedStorage = memoryStorage();
  tamperedStorage.write('ignored', {
    items: [{
      eventKey: 'device-new/5/3',
      deviceId: 'device-old',
      wearableId: 'device-old',
      boot: 5,
      id: 3,
      alarmType: 1,
      phase: 'active'
    }]
  });
  const sanitized = createGuardianOutbox({ storage: tamperedStorage, send: () => { throw new Error('不应调用'); } });
  assert.strictEqual(sanitized.getState().pending, 0, '恢复时 eventKey 与设备字段不一致的条目必须丢弃');

  const mismatch = createGuardianOutbox({ storage: memoryStorage(), send: () => { throw new Error('不应调用'); } });
  assert.strictEqual(mismatch.enqueue({ ...active, deviceId: 'device-a', wearableId: 'device-b' }).reason, 'invalid-event', '设备别名不一致时不得入队');
  mismatch.enqueue({ ...active, deviceId: 'device-old' });
  const skipped = await mismatch.flush({ canSend: (event) => event.deviceId === 'device-new' ? true : { ok: false, reason: 'binding-device-mismatch' } });
  assert.strictEqual(skipped.skipped, true);
  assert.strictEqual(skipped.reason, 'binding-device-mismatch');
  assert.strictEqual(mismatch.getState().pending, 1, '设备绑定不符时不得删除本地队列');

  const bounded = createGuardianOutbox({ storage: memoryStorage(), maxItems: 2, send: () => Promise.resolve() });
  bounded.enqueue(active);
  bounded.enqueue(resolved);
  assert.strictEqual(bounded.enqueue({ ...active, deviceId: 'device-b' }).reason, 'outbox-full');

  console.log('guardian outbox tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
