const assert = require('assert');
const { createAppState } = require('../store/app-state');
const { ROLES } = require('../services/guardian-binding');

function clone(value) {
  return value === undefined ? value : JSON.parse(JSON.stringify(value));
}

function memoryStorage() {
  let value;
  return {
    read: () => clone(value),
    write: (key, next) => { value = clone(next); },
    value: () => clone(value)
  };
}

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

async function main() {
  let online = false;
  const cloudEvents = [];
  const ackCommands = [];
  let bluetoothCallbacks;
  const alarmAudio = {
    playCount: 0,
    stopCount: 0,
    play() { this.playCount += 1; return Promise.resolve({ ok: true, local: true }); },
    stop() { this.stopCount += 1; }
  };
  const guardianApi = {
    config: { pollIntervalMs: 3000, offlineAfterMs: 60000, eventPageSize: 50 },
    isConfigured: () => true,
    publishEvent(event) {
      if (!online) return Promise.reject(new Error('offline'));
      cloudEvents.push({ ...event });
      return Promise.resolve({ ok: true });
    },
    acknowledgeEvent: () => Promise.resolve({ ok: true }),
    touchStatus: () => Promise.resolve({ ok: true }),
    getBinding: () => Promise.resolve({ role: ROLES.WEARER, binding: { bindingId: 'binding-a', wearableId: 'device-a' } }),
    listEvents: () => Promise.resolve({ events: [] }),
    getStatus: () => Promise.resolve({ status: { wearableId: 'device-a', online: true, lastSeenAt: Date.now() } })
  };
  const binding = {
    getState: () => ({ configured: true, role: ROLES.WEARER, binding: { bindingId: 'binding-a', wearableId: 'device-a' }, invite: null }),
    selectRole: (role) => ({ configured: true, role, binding: { bindingId: 'binding-a', wearableId: 'device-a' }, invite: null }),
    refresh: () => Promise.resolve(binding.getState()),
    createInvite: () => Promise.resolve(binding.getState()),
    acceptInvite: () => Promise.resolve(binding.getState())
  };
  const fakeBluetooth = {
    startDiscovery: () => Promise.resolve(),
    connect: () => Promise.resolve(),
    disconnect: () => Promise.resolve(),
    closeAdapter: () => Promise.resolve(),
    write: (value) => {
      const command = Buffer.from(value).toString('utf8');
      ackCommands.push(command);
      return Promise.resolve();
    }
  };
  const runtime = createAppState({
    alarmAudio,
    guardianApi,
    guardianBinding: binding,
    alarmStorage: memoryStorage(),
    guardianOutboxStorage: memoryStorage(),
    guardianOutboxBaseDelayMs: 0,
    alarmAckTimeoutMs: 30,
    alarmAckMaxAttempts: 2,
    bluetoothFactory: (callbacks) => { bluetoothCallbacks = callbacks; return fakeBluetooth; }
  });
  bluetoothCallbacks.onStateChange({ adapterReady: true, connected: true, deviceId: 'device-a', message: 'mock connected' });

  const flexAtBefore = runtime.getState().lastFlexAt;
  runtime.ingestRawData(Buffer.from('ACC|X=0.01|Y=0.02|Z=1.00|VALID=0\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().acc.valid, false);
  assert.strictEqual(runtime.getState().accHealth.valid, false);
  assert.strictEqual(runtime.getState().lastFlexAt, flexAtBefore, 'ACC 不能更新 FLEX 新鲜度');

  runtime.ingestRawData(Buffer.from('BRINGUP: JY_R=0,JY_L=1,JY_R_RET=1,JY_L_RET=0,MAX=1,MAX_RET=0,MAX_PART=0x57,MAX_HAL_ERR=0,ADC1=1,ADC2=1,DEG=1\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().jyValid, false, '当前固件双 JY BRINGUP 应更新诊断状态');
  runtime.ingestRawData(Buffer.from('JY|ONLINE=0|ERR=3|LAST=1|AGE=4294967295\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().jyValid, false, '动态 JY 离线状态必须进入运行时');
  assert.strictEqual(runtime.getState().jyStatus.sampleAgeMs, 0xFFFFFFFF);

  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=1|TYPE=1|ACTIVE=1\r\n', 'utf8'));
  await tick();
  let state = runtime.getState();
  assert.strictEqual(state.jyValid, false);
  assert.strictEqual(state.alarm.active.length, 1, '有效 ALARM 不得因 JY 状态未确认而丢弃');
  assert.strictEqual(alarmAudio.playCount, 1);
  assert.strictEqual(state.guardian.outbox.pending, 1, '云端失败时 ACTIVE 必须进入持久化 outbox');

  const writeCountBeforeLocalAck = ackCommands.length;
  const localAck = await runtime.acknowledgeAlarm('device-a/42/1', 'local');
  assert.strictEqual(localAck.accepted, true, '本地确认应只更新手机侧确认状态');
  assert.strictEqual(ackCommands.length, writeCountBeforeLocalAck,
    '停止本地提示不得写入 FFE1 或触发设备解除');
  assert.strictEqual(runtime.getState().alarm.active.length, 1,
    '本地确认不得清除固件活动事件');

  const connectedHide = await runtime.hideLocalAlarm('device-a/42/1');
  assert.strictEqual(connectedHide.reason, 'device-connected', '已连接设备时不能把当前设备报警仅隐藏在本机');
  bluetoothCallbacks.onStateChange({ adapterReady: true, connected: false, deviceId: 'device-a', message: 'mock disconnected' });
  const hidden = await runtime.hideLocalAlarm('device-a/42/1');
  assert.strictEqual(hidden.accepted, true, '断连且本机确认后可隐藏无法立即核验的旧提示');
  assert.strictEqual(runtime.getState().alarm.active.length, 0, '隐藏后当前本机界面不再展示旧卡片');
  assert.strictEqual(runtime.getState().alarm.allActive.length, 1, '隐藏不能伪造设备 ACTIVE=0');
  assert.strictEqual(ackCommands.length, writeCountBeforeLocalAck,
    '隐藏旧提示不得写入 FFE1 或触发设备解除');
  bluetoothCallbacks.onStateChange({ adapterReady: true, connected: true, deviceId: 'device-a', message: 'mock reconnected' });
  runtime.ingestRawData(Buffer.from('ALARM_STATE|BOOT=42|ACTIVE=1|ID=1|TYPE=1\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().alarm.active.length, 1,
    '重连后设备仍确认 ACTIVE=1 时必须重新展示旧提示');

  runtime.ingestRawData(Buffer.from('ALARM_STATE|BOOT=42|ACTIVE=0|ID=0|TYPE=0\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().alarm.active.length, 0,
    '固件状态心跳 ACTIVE=0 必须清理丢失回执后残留的活动事件');

  // 断线重连会清空上一条连接的动态 JY 诊断；随后收到的当前连接离线帧才是有效依据。
  runtime.ingestRawData(Buffer.from('JY|ONLINE=0|ERR=3|LAST=1|AGE=4294967295\r\n', 'utf8'));

  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=1|TYPE=1|ACTIVE=1\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().alarm.active.length, 0,
    '同一事件已由状态心跳解除后不得被旧的重复 ACTIVE=1 重新激活');
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=2|TYPE=1|ACTIVE=1\r\n', 'utf8'));

  runtime.ingestRawData(Buffer.from('FLEX|L1=10|L2=20|L3=30|L4=40|L5=50|R1=60|R2=70|R3=80|R4=90|R5=100\r\n', 'utf8'));
  const flexAt = runtime.getState().lastFlexAt;
  assert.ok(flexAt > 0);
  runtime.ingestRawData(Buffer.from('BRINGUP: JY_R=1,JY_L=1,JY_R_RET=0,JY_L_RET=0,MAX=1,MAX_RET=0,MAX_PART=0x57,MAX_HAL_ERR=0,ADC1=1,ADC2=1,DEG=0\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().jyValid, false, '启动 BRINGUP JY=1 不得覆盖动态 JY 离线状态');
  runtime.ingestRawData(Buffer.from('JY|ONLINE=1|ERR=0|LAST=0|AGE=87\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().jyValid, true, '动态 JY 在线且样本新鲜时应恢复有效状态');
  runtime.ingestRawData(Buffer.from('IMU|R=12.50|P=-3.25|Y=47.00\r\n', 'utf8'));
  runtime.ingestRawData(Buffer.from('JY|ONLINE=1|ERR=0|LAST=16|AGE=88\r\n', 'utf8'));
  state = runtime.getState();
  assert.strictEqual(state.jyValid, false,
    '全零姿态快照不能作为跌倒/抽搐判定的有效 JY 样本');
  assert.strictEqual(state.jyStatus.lastErrorMask, 16);
  assert.deepStrictEqual(state.rawPose, { roll: 12.5, pitch: -3.25, yaw: 47 },
    '诊断帧标记全零快照时必须保留最近有效的 IMU 展示值');
  runtime.ingestRawData(Buffer.from('ACC|X=0|Y=0|Z=1|VALID=1\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().lastFlexAt, flexAt, 'ACC 不能覆盖已收到的 FLEX 时间');

  const resolveRequest = runtime.requestAlarmResolve('device-a/42/2');
  const duplicateResolve = runtime.requestAlarmResolve('device-a/42/2');
  const duplicateResult = await duplicateResolve;
  assert.strictEqual(duplicateResult.reason, 'already-pending',
    '重复点击同一事件不得创建第二个解除请求');
  await tick();
  assert.deepStrictEqual(ackCommands, ['ALARM_ACK:42:2\n']);
  state = runtime.getState();
  assert.strictEqual(state.alarmAckStatus.stage, 'waiting',
    'BLE 写入成功后 UI 必须显示等待设备回执');
  assert.strictEqual(state.alarmAckStatus.key, 'device-a/42/2');
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=2|TYPE=1|ACTIVE=0\r\n', 'utf8'));
  const resolveResult = await resolveRequest;
  assert.strictEqual(resolveResult.accepted, true);
  state = runtime.getState();
  assert.strictEqual(state.alarm.active.length, 0, '只有当前设备 ACTIVE=0 才能清除活动事件');
  assert.strictEqual(state.alarmAckStatus.stage, 'resolved',
    '只有匹配 ACTIVE=0 回帧后 UI 才能显示设备已解除');
  assert.strictEqual(state.guardian.outbox.pending, 4,
    '状态心跳恢复和设备解除都必须保留 ACTIVE/RESOLVE 两阶段');

  await tick(); // 等待离线发送（含认证绑定查询）完成，再模拟网络恢复。
  online = true;
  const flushed = await runtime.flushGuardianOutbox();
  assert.strictEqual(flushed.ok, true, JSON.stringify({ flushed, outbox: runtime.getGuardianOutbox().getState() }));
  assert.deepStrictEqual(cloudEvents.map((event) => event.active), [true, false, true, false]);
  assert.strictEqual(runtime.getState().guardian.outbox.pending, 0);

  // 设备重启后，旧会话的 ACTIVE=0 不能清除仍展示的旧活动事件，也不能发送旧 ACK。
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=43|ID=2|TYPE=2|ACTIVE=1\r\n', 'utf8'));
  await tick();
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=1|TYPE=1|ACTIVE=0\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().alarm.active.some((item) => item.eventKey === 'device-a/43/2'), true,
    '旧 BOOT 的 ACTIVE=0 不得清除新启动会话的活动事件');
  runtime.ingestRawData(Buffer.from('BOOT:STM32F407_BASE\r\n', 'utf8'));
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=43|ID=2|TYPE=2|ACTIVE=0\r\n', 'utf8'));
  state = runtime.getState();
  assert.strictEqual(state.alarm.active.some((item) => item.eventKey === 'device-a/43/2'), true,
    'BOOT 后迟到的旧 ACTIVE=0 不得清除活动事件');
  const staleAfterBoot = await runtime.requestAlarmResolve('device-a/43/2');
  assert.strictEqual(staleAfterBoot.reason, 'stale-device',
    'BOOT 后旧事件不得再次请求设备解除');
  assert.deepStrictEqual(ackCommands, ['ALARM_ACK:42:2\n']);

  // 历史旧设备事件不能借新连接发送 ACK。
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=99|TYPE=2|ACTIVE=1\r\n', 'utf8'));
  bluetoothCallbacks.onStateChange({ connected: true, deviceId: 'device-b', message: 'switched device' });
  const stale = await runtime.requestAlarmResolve('device-a/42/99');
  assert.strictEqual(stale.reason, 'stale-device');
  assert.deepStrictEqual(ackCommands, ['ALARM_ACK:42:2\n']);

  const historyBeforeStaleFrame = runtime.getState().alarm.history.length;
  bluetoothCallbacks.onValueChange(Buffer.from('ALARM|BOOT=42|ID=3|TYPE=1|ACTIVE=1\r\n', 'utf8'), { deviceId: 'device-a' });
  assert.strictEqual(runtime.getState().alarm.history.length, historyBeforeStaleFrame, '回调标识为旧设备时不得注入新连接');
  bluetoothCallbacks.onStateChange({ connected: false, deviceId: 'device-b', message: 'mock disconnected' });
  runtime.ingestRawData(Buffer.from('ALARM|BOOT=42|ID=4|TYPE=1|ACTIVE=1\r\n', 'utf8'));
  assert.strictEqual(runtime.getState().alarm.history.length, historyBeforeStaleFrame, '断开后迟到 ALARM 不得进入本地事件');

  runtime.onAppHide();
  assert.strictEqual(runtime.getState().guardian.foreground, false);

  // 监护手机没有 BLE：服务端确认的佩戴者 ACTIVE=0 只能同步监护端展示，不能伪装成固件回帧。
  let guardianEvents = [{
    eventKey: 'device-a/43/1',
    wearableId: 'device-a',
    boot: 43,
    id: 1,
    alarmType: 1,
    active: true,
    origin: 'device',
    deviceConfirmed: true,
    acknowledged: false
  }];
  const guardianWrites = [];
  const guardianAudio = { playCount: 0, stopCount: 0, play() { this.playCount += 1; return Promise.resolve({ ok: true }); }, stop() { this.stopCount += 1; } };
  let guardianCallbacks;
  const guardianBindingState = { bindingId: 'binding-a', wearableId: 'device-a' };
  const guardianBinding = {
    getState: () => ({ configured: true, role: ROLES.GUARDIAN, binding: guardianBindingState, invite: null }),
    refresh: () => Promise.resolve({ configured: true, role: ROLES.GUARDIAN, binding: guardianBindingState, invite: null }),
    selectRole: () => guardianBinding.getState(),
    createInvite: () => Promise.resolve(guardianBinding.getState()),
    acceptInvite: () => Promise.resolve(guardianBinding.getState())
  };
  const guardianApiForMonitor = {
    config: { pollIntervalMs: 3000, offlineAfterMs: 60000, eventPageSize: 50 },
    isConfigured: () => true,
    listEvents: () => Promise.resolve({ events: guardianEvents.map((event) => ({ ...event })) }),
    getStatus: () => Promise.resolve({ status: { wearableId: 'device-a', online: true, lastSeenAt: Date.now() } }),
    getBinding: () => Promise.resolve({ role: ROLES.GUARDIAN, binding: guardianBindingState }),
    publishEvent: () => Promise.resolve({ ok: true }),
    acknowledgeEvent: () => Promise.resolve({ ok: true }),
    touchStatus: () => Promise.resolve({ ok: true })
  };
  const guardianRuntime = createAppState({
    alarmAudio: guardianAudio,
    guardianApi: guardianApiForMonitor,
    guardianBinding,
    alarmStorage: memoryStorage(),
    guardianOutboxStorage: memoryStorage(),
    bluetoothFactory: (callbacks) => {
      guardianCallbacks = callbacks;
      return {
        writeText: (command) => { guardianWrites.push(command); return Promise.resolve(); },
        startDiscovery: () => Promise.resolve(),
        connect: () => Promise.resolve(),
        disconnect: () => Promise.resolve(),
        closeAdapter: () => Promise.resolve()
      };
    }
  });
  await guardianRuntime.startGuardianMonitor();
  state = guardianRuntime.getState();
  assert.strictEqual(state.alarm.active.length, 1, '监护端应展示服务端确认的活动事件');
  assert.strictEqual(state.alarm.active[0].deviceConfirmed, false, '云端确认不能冒充监护端的 BLE 固件回帧');
  assert.strictEqual(state.alarm.active[0].cloudConfirmed, true, '服务端 deviceConfirmed=true 应允许监护端同步状态');

  guardianEvents = [{ ...guardianEvents[0], acknowledged: true, active: true }];
  await guardianRuntime.pollGuardian();
  assert.strictEqual(guardianRuntime.getState().alarm.active.length, 1, '监护者 ack-only 不能解除活动事件');

  guardianEvents = [{ ...guardianEvents[0], active: false, deviceConfirmed: false }];
  await guardianRuntime.pollGuardian();
  assert.strictEqual(guardianRuntime.getState().alarm.active.length, 1, '服务端未确认的 ACTIVE=0 不能解除监护端事件');

  guardianEvents = [{ ...guardianEvents[0], wearableId: 'device-other', active: false, deviceConfirmed: true }];
  await guardianRuntime.pollGuardian();
  assert.strictEqual(guardianRuntime.getState().alarm.active.length, 1, '非当前 binding 设备的云事件必须被忽略');

  const listEventsBefore = guardianApiForMonitor.listEvents;
  const refreshBefore = guardianBinding.refresh;
  let releaseQuery;
  guardianApiForMonitor.listEvents = () => new Promise((resolve) => { releaseQuery = resolve; });
  const lateQuery = guardianRuntime.pollGuardian();
  await tick();
  guardianBinding.refresh = () => Promise.resolve({ configured: true, role: ROLES.GUARDIAN,
    binding: { bindingId: 'binding-new', wearableId: 'device-a' } });
  await guardianRuntime.refreshGuardian();
  releaseQuery({ events: [{ ...guardianEvents[0], wearableId: 'device-a', active: false, deviceConfirmed: true }] });
  await lateQuery;
  assert.strictEqual(guardianRuntime.getState().alarm.active.length, 1, '旧绑定查询迟到不能在新绑定上下文消警');
  guardianApiForMonitor.listEvents = listEventsBefore;
  guardianBinding.refresh = refreshBefore;
  await guardianRuntime.refreshGuardian();

  guardianEvents = [{ ...guardianEvents[0], active: false, acknowledged: true, deviceConfirmed: true }];
  guardianEvents[0].wearableId = 'device-a';
  await guardianRuntime.pollGuardian();
  state = guardianRuntime.getState();
  assert.strictEqual(state.alarm.active.length, 0, '云端已确认的佩戴者 ACTIVE=0 可同步监护端 resolved');
  const cloudResolved = state.alarm.history.find((event) => event.eventKey === 'device-a/43/1');
  assert.strictEqual(cloudResolved.status, 'resolved');
  assert.strictEqual(cloudResolved.cloudConfirmed, true);
  assert.strictEqual(cloudResolved.deviceConfirmed, false, '监护端 resolved 仍不能声明收到 BLE 回帧');
  assert.deepStrictEqual(guardianWrites, [], '监护端同步 resolved 不能发送 BLE 或解除佩戴者本地状态');
  guardianRuntime.stopGuardianMonitor();

  // BLE 断开后重启/恢复网络：合法历史 outbox 按已认证的 binding 上下文补发，不依赖设备在线。
  let networkOnline = false;
  let currentBinding = { bindingId: 'binding-a', wearableId: 'device-a' };
  let getBindingCalls = 0;
  let wearerCallbacks;
  const wearerSent = [];
  const wearerOutboxStorage = memoryStorage();
  const wearerBinding = {
    getState: () => ({ configured: true, role: ROLES.WEARER, binding: currentBinding, invite: null }),
    refresh: () => wearerApi.getBinding().then((result) => ({ configured: true, role: result.role, binding: result.binding, invite: null })),
    selectRole: (role) => ({ configured: true, role, binding: currentBinding, invite: null }),
    createInvite: () => Promise.resolve(wearerBinding.getState()),
    acceptInvite: () => Promise.resolve(wearerBinding.getState())
  };
  const wearerApi = {
    config: { pollIntervalMs: 3000, offlineAfterMs: 60000, eventPageSize: 50 },
    isConfigured: () => true,
    publishEvent: (event) => {
      if (!networkOnline) return Promise.reject(new Error('offline'));
      wearerSent.push({ ...event });
      return Promise.resolve({ ok: true });
    },
    getBinding: () => {
      getBindingCalls += 1;
      return Promise.resolve({ role: ROLES.WEARER, binding: currentBinding });
    },
    listEvents: () => Promise.resolve({ events: [] }),
    getStatus: () => Promise.resolve({ status: { wearableId: currentBinding.wearableId, online: true, lastSeenAt: Date.now() } }),
    acknowledgeEvent: () => Promise.resolve({ ok: true }),
    touchStatus: () => Promise.resolve({ ok: true })
  };
  const wearerRuntime = createAppState({
    alarmAudio,
    guardianApi: wearerApi,
    guardianBinding: wearerBinding,
    alarmStorage: memoryStorage(),
    guardianOutboxStorage: wearerOutboxStorage,
    guardianOutboxBaseDelayMs: 0,
    bluetoothFactory: (callbacks) => {
      wearerCallbacks = callbacks;
      return {
        writeText: () => Promise.resolve(),
        startDiscovery: () => Promise.resolve(),
        connect: () => Promise.resolve(),
        disconnect: () => Promise.resolve(),
        closeAdapter: () => Promise.resolve()
      };
    }
  });
  wearerCallbacks.onStateChange({ adapterReady: true, connected: true, deviceId: 'device-a', message: 'connected' });
  wearerRuntime.ingestRawData(Buffer.from('ALARM|BOOT=44|ID=1|TYPE=1|ACTIVE=1\r\n', 'utf8'));
  await tick();
  assert.strictEqual(wearerOutboxStorage.value().items.length, 1, '网络失败时合法 BLE 事件必须持久化');
  wearerCallbacks.onStateChange({ connected: false, deviceId: 'device-a', message: 'disconnected' });
  wearerRuntime.onAppHide();
  networkOnline = true;

  const restoredWearer = createAppState({
    alarmAudio,
    guardianApi: wearerApi,
    guardianBinding: wearerBinding,
    alarmStorage: memoryStorage(),
    guardianOutboxStorage: wearerOutboxStorage,
    guardianOutboxBaseDelayMs: 0,
    bluetoothFactory: () => ({
      writeText: () => Promise.resolve(),
      startDiscovery: () => Promise.resolve(),
      connect: () => Promise.resolve(),
      disconnect: () => Promise.resolve(),
      closeAdapter: () => Promise.resolve()
    })
  });
  await restoredWearer.onAppShow();
  assert.deepStrictEqual(wearerSent.map((event) => event.active), [true], '断开 BLE 后网络恢复应补发合法旧 ACTIVE');
  assert.strictEqual(restoredWearer.getState().guardian.outbox.pending, 0);

  // 换绑后即使设备标识仍来自旧事件，也不能用新账号 binding 发送。
  let restoredCallbacks;
  const rebound = createAppState({
    alarmAudio,
    guardianApi: wearerApi,
    guardianBinding: wearerBinding,
    alarmStorage: memoryStorage(),
    guardianOutboxStorage: memoryStorage(),
    guardianOutboxBaseDelayMs: 0,
    bluetoothFactory: (callbacks) => {
      restoredCallbacks = callbacks;
      return {
        writeText: () => Promise.resolve(),
        startDiscovery: () => Promise.resolve(),
        connect: () => Promise.resolve(),
        disconnect: () => Promise.resolve(),
        closeAdapter: () => Promise.resolve()
      };
    }
  });
  networkOnline = false;
  currentBinding = { bindingId: 'binding-a', wearableId: 'device-a' };
  restoredCallbacks.onStateChange({ connected: true, deviceId: 'device-a', message: 'connected' });
  rebound.ingestRawData(Buffer.from('ALARM|BOOT=44|ID=2|TYPE=1|ACTIVE=1\r\n', 'utf8'));
  await tick();
  restoredCallbacks.onStateChange({ connected: false, deviceId: 'device-a', message: 'disconnected' });
  networkOnline = true;
  currentBinding = { bindingId: 'binding-b', wearableId: 'device-a' };
  const bindingCallsBeforeSend = getBindingCalls;
  await rebound.flushGuardianOutbox();
  assert.ok(getBindingCalls > bindingCallsBeforeSend, '历史 outbox 发送前必须通过认证 getBinding 确认当前 binding');
  assert.strictEqual(rebound.getState().guardian.binding.bindingId, 'binding-b', '发送前确认应更新过期的本地 binding 缓存');
  assert.strictEqual(wearerSent.length, 1, '换绑后旧 binding/device 事件不能发送给新账号');
  assert.strictEqual(rebound.getState().guardian.outbox.pending, 1, '换绑不匹配的历史事件必须保留而非误发');
  currentBinding = { bindingId: 'binding-a', wearableId: 'device-a' };
  await rebound.refreshGuardian();
  const savedGetBinding = wearerApi.getBinding;
  wearerApi.getBinding = () => Promise.reject(new Error('binding lookup offline'));
  await rebound.flushGuardianOutbox();
  assert.strictEqual(wearerSent.length, 1, '认证查询失败不能回退到旧缓存发送');
  assert.strictEqual(rebound.getState().guardian.outbox.pending, 1);
  wearerApi.getBinding = savedGetBinding;

  console.log('alarm integration tests passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
