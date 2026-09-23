const { createBluetoothClient } = require('../utils/bluetooth');
const { createProtocolParser } = require('../utils/protocol');
const { classifyFingers } = require('../services/gesture-matcher');
const { createSafetyMonitor } = require('../services/safety-monitor');
const { createCalibrationStore, validFlex } = require('../services/calibration');
const { createAlarmAudio } = require('../services/alarm-audio');
const { createAlarmRuntime } = require('../services/alarm-runtime');
const { createGuardianApi } = require('../services/guardian-api');
const { createGuardianBindingService, ROLES } = require('../services/guardian-binding');
const { createGuardianMonitor } = require('../services/guardian-monitor');
const { createGuardianOutbox } = require('../services/guardian-outbox');
const { createAlarmAckController } = require('../services/alarm-ack');
const { createSimulator } = require('../services/simulator');

const DATA_LOG_INTERVAL_MS = 1000;
const JY_MAX_SAMPLE_AGE_MS = 5000;
const JY_CURRENT_SAMPLE_OK = 0;

function emptyJYStatus() {
  return {
    source: 'none',
    online: false,
    errorStreak: null,
    lastErrorMask: null,
    sampleAgeMs: null,
    stale: false,
    receivedAt: 0
  };
}

function encodeUtf8(text) {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(text).buffer;
  const encoded = unescape(encodeURIComponent(text));
  const bytes = new Uint8Array(encoded.length);
  for (let index = 0; index < encoded.length; index += 1) bytes[index] = encoded.charCodeAt(index);
  return bytes.buffer;
}

const MODES = Object.freeze({
  translation: 'translation',
  rehabilitation: 'rehabilitation',
  remote: 'remote'
});

function emptyState() {
  return {
    mode: MODES.translation,
    adapterReady: false,
    discovering: false,
    connecting: false,
    connected: false,
    reconnecting: false,
    statusText: 'Bluetooth disconnected',
    deviceName: '',
    deviceId: '',
    serviceId: '',
    characteristicId: '',
    notifyEnabled: false,
    devices: [],
    rawFlex: Array(10).fill(null),
    flex: Array(10).fill(null),
    fingerStates: Array(10).fill(null),
    rawPose: { roll: null, pitch: null, yaw: null },
    pose: { roll: null, pitch: null, yaw: null },
    acc: { x: null, y: null, z: null, valid: false, unit: 'g', lastAt: 0 },
    accHealth: { valid: false, lastAt: 0, statusText: '尚未收到 ACC 诊断帧' },
    bringup: null,
    jyValid: false,
    jyStatus: emptyJYStatus(),
    bootName: '',
    care: { hr: 0, spo2: 0, fall: false, sos: false, source: 'demo' },
    ppg: { ir: 0, red: 0, valid: false, hr: null, spo2: null },
    calibration: { flexZero: null, flexFull: null, imuZero: null, enabledFingers: Array(10).fill(true) },
    lastFrameAt: 0,
    lastFlexAt: 0,
    lastRawFrame: '',
    bleLogs: [],
    lastError: '',
    simulator: { active: false, running: false, intervalMs: 250, lastInjectedAt: 0, snapshot: { flex: Array(10).fill(0), imu: { roll: 0, pitch: 0, yaw: 0 }, acc: { x: 0, y: 0, z: 1 } } },
    safetyAlert: null,
    buzzerStatus: '',
    alarmAckStatus: { stage: 'idle', key: '', command: '', attempts: 0, maxAttempts: 0, detail: '' },
    alarm: { active: [], allActive: [], hiddenActiveCount: 0, history: [], latest: null },
    alarmAudioStatus: '本地报警音待触发',
    guardian: {
      configured: false,
      role: ROLES.UNSELECTED,
      binding: null,
      invite: null,
      online: false,
      lastSeenAt: 0,
      statusText: '监护服务未配置',
      lastError: '',
      foreground: true,
      outbox: { pending: 0, items: [], lastError: '', lastFlushAt: 0, nextRetryAt: 0, maxItems: 50 }
    },
    recognition: { name: '', text: '等待手势', status: '正在等待完整十指数据' }
  };
}

function createAppState(options = {}) {
  const calibrationStore = options.calibrationStore || createCalibrationStore();
  const state = { ...emptyState(), calibration: calibrationStore.read() };
  const listeners = new Set();
  const safety = options.safety || createSafetyMonitor();
  const audioOptions = { ...(options.alarmAudioOptions || {}) };
  const suppliedAudioError = audioOptions.onError;
  const guardianApi = options.guardianApi || createGuardianApi({ wx: options.wx, config: options.guardianConfig });
  const guardianConfig = guardianApi.config || {};
  const alarmAudio = options.alarmAudio || createAlarmAudio({
    ...audioOptions,
    wx: options.wx,
    onError: (result) => {
      if (typeof suppliedAudioError === 'function') suppliedAudioError(result);
      emit({ alarmAudioStatus: `本地报警音播放失败：${result.detail || result.reason}` });
      appendLog('error', `本地报警音播放失败：${result.detail || result.reason}`);
    }
  });
  const guardianBinding = options.guardianBinding || createGuardianBindingService({ api: guardianApi, storage: options.guardianBindingStorage });
  const bindingState = guardianBinding.getState();
  state.guardian = {
    ...state.guardian,
    ...bindingState,
    statusText: bindingState.configured ? '监护服务已配置，等待绑定状态' : '监护服务未配置：请填写环境 ID 并部署云函数。'
  };
  let pendingDataTimer = null;
  let lastDataLogAt = 0;
  let lastGuardianStatusAt = 0;
  let lastDeviceId = '';
  let connectionGeneration = 0;
  let currentDeviceBoot = null;
  let dynamicJYReceived = false;
  let appForeground = true;
  let bluetooth;
  let guardianOutbox;

  function updateGuardian(patch) {
    emit({ guardian: { ...state.guardian, ...patch } });
  }

  const cloudSnapshotToken = {};
  const alarmRuntime = createAlarmRuntime({
    authorizeCloudSnapshot: (event, context) => context.cloudSnapshotToken === cloudSnapshotToken
      && state.guardian.role === ROLES.GUARDIAN && context.cloudConfirmed === true,
    onAttention: (event) => {
      if (typeof wx !== 'undefined' && wx.vibrateLong) wx.vibrateLong();
      alarmAudio.play().then((result) => {
        if (result.reason === 'cancelled') return;
        emit({ alarmAudioStatus: result.ok ? '本地报警音正在播放（系统音量）' : `本地报警音不可用：${result.detail || result.reason}` });
      }).catch((error) => emit({ alarmAudioStatus: `本地报警音不可用：${error.message || error}` }));
    },
    onAcknowledged: () => {
      const alarmState = alarmRuntime.getState();
      const active = alarmState.allActive || alarmState.active;
      if (active.every((item) => item.acknowledgedBy && item.acknowledgedBy.length)) alarmAudio.stop();
      emit({ alarmAudioStatus: active.every((item) => item.acknowledgedBy && item.acknowledgedBy.length)
        ? '本地报警音已停止；活动事件仍等待固件 ACTIVE=0。'
        : '本事件已确认；仍有未确认活动报警。' });
    },
    onResolved: (event) => {
      const alarmState = alarmRuntime.getState();
      if (!(alarmState.allActive || alarmState.active).length) alarmAudio.stop();
      emit({ alarmAudioStatus: event && event.cloudConfirmed && !event.deviceConfirmed
        ? '云端已确认佩戴者 ACTIVE=0，监护端已同步解除。'
        : '固件已发送 ACTIVE=0，报警已解除。' });
    },
    onAllResolved: () => {
      alarmAudio.stop();
      emit({ alarmAudioStatus: '当前没有活动报警。' });
    }
  });
  alarmRuntime.subscribe((alarm) => emit({ alarm, safetyAlert: alarm.active[0] || null }));

  function guardianErrorText(error) {
    return error && (error.message || error.errMsg || error.msg) || String(error || '监护服务错误');
  }

  function recordGuardianError(error) {
    const message = guardianErrorText(error);
    updateGuardian({ lastError: message, statusText: message });
    appendLog('error', `监护服务：${message}`);
    return error;
  }

  function touchGuardianStatusIfNeeded() {
    if (!appForeground || !state.connected || state.guardian.role !== ROLES.WEARER || !state.guardian.binding || !guardianApi.isConfigured()) return;
    const wearableId = state.deviceId || '';
    const boundDeviceId = String(state.guardian.binding.wearableId || state.guardian.binding.deviceId || '').trim();
    if (!wearableId || !boundDeviceId || wearableId !== boundDeviceId) return;
    if (!wearableId || Date.now() - lastGuardianStatusAt < 30000) return;
    lastGuardianStatusAt = Date.now();
    guardianApi.touchStatus({ wearableId, online: true }).catch(recordGuardianError);
  }

  function guardianBindingEligibility(event) {
    if (state.guardian.role !== ROLES.WEARER) return { ok: false, reason: 'role-not-wearer' };
    if (!state.guardian.binding) return { ok: false, reason: 'binding-missing' };
    const currentDeviceId = String(state.deviceId || '').trim();
    const boundDeviceId = String(state.guardian.binding.wearableId || state.guardian.binding.deviceId || '').trim();
    if (!currentDeviceId) return { ok: false, reason: 'device-not-connected' };
    if (!boundDeviceId || boundDeviceId !== currentDeviceId) return { ok: false, reason: 'binding-device-mismatch' };
    const currentBindingId = String(state.guardian.binding.bindingId || '').trim();
    if (!currentBindingId) return { ok: false, reason: 'binding-id-missing' };
    if (event && String(event.deviceId || event.wearableId || '').trim() !== currentDeviceId) {
      return { ok: false, reason: 'event-device-mismatch' };
    }
    if (event && event.bindingId && event.bindingId !== currentBindingId) return { ok: false, reason: 'binding-mismatch' };
    return { ok: true };
  }

  function guardianLiveEventEligibility(event) {
    if (!state.connected) return { ok: false, reason: 'device-not-connected' };
    const bindingEligibility = guardianBindingEligibility(event);
    if (!bindingEligibility.ok) return bindingEligibility;
    if (!event || event.source !== 'ble' || event.origin !== 'device' || event.deviceConfirmed !== true) {
      return { ok: false, reason: 'untrusted-event-source' };
    }
    return { ok: true };
  }

  function guardianPublishEligibility(event) {
    if (state.guardian.role !== ROLES.WEARER) return { ok: false, reason: 'role-not-wearer' };
    if (!state.guardian.binding) return { ok: false, reason: 'binding-missing' };
    if (!guardianApi.isConfigured()) return { ok: false, reason: 'not-configured' };
    const currentBindingId = String(state.guardian.binding.bindingId || '').trim();
    const boundDeviceId = String(state.guardian.binding.wearableId || state.guardian.binding.deviceId || '').trim();
    if (!currentBindingId) return { ok: false, reason: 'binding-id-missing' };
    if (!boundDeviceId) return { ok: false, reason: 'binding-device-mismatch' };
    if (event) {
      const eventDeviceId = String(event.deviceId || event.wearableId || '').trim();
      if (!eventDeviceId || eventDeviceId !== boundDeviceId) return { ok: false, reason: 'binding-device-mismatch' };
      const eventBindingId = String(event.bindingId || '').trim();
      if (!eventBindingId) return { ok: false, reason: 'binding-id-missing' };
      if (eventBindingId !== currentBindingId) return { ok: false, reason: 'binding-mismatch' };
    }
    return { ok: true };
  }

  async function sendGuardianOutboxItem(event, item) {
    // 每项发送前读取认证账号绑定；本地缓存和 payload bindingId 都不是授权依据。
    const confirmed = await guardianApi.getBinding();
    if (!appForeground) throw new Error('监护上报已阻止：background');
    updateGuardian({ role: confirmed.role, binding: confirmed.binding || null });
    const eligibility = guardianPublishEligibility({ ...event, bindingId: item && item.bindingId });
    if (!eligibility.ok) {
      const error = new Error(`监护上报已阻止：${eligibility.reason}`);
      error.code = 'GUARDIAN_PUBLISH_NOT_ELIGIBLE';
      return Promise.reject(error);
    }
    return guardianApi.publishEvent(event);
  }

  function publishGuardianEvent(event) {
    if (state.guardian.role !== ROLES.WEARER) return Promise.resolve({ skipped: true, reason: 'role-not-wearer' });
    const bindingEligibility = guardianLiveEventEligibility(event);
    if (!bindingEligibility.ok) return Promise.resolve({ skipped: true, reason: bindingEligibility.reason });
    const bindingId = String(state.guardian.binding.bindingId || '').trim();
    const queued = guardianOutbox.enqueue({ ...event, bindingId });
    if (!queued.accepted && queued.reason !== 'deduplicated') {
      updateGuardian({ outbox: guardianOutbox.getState(), lastError: queued.detail || `监护事件未入队：${queued.reason}` });
      return Promise.resolve(queued);
    }
    if (!appForeground || !queued.accepted) return Promise.resolve(queued);
    return flushGuardianOutbox().then((flushed) => ({ ...queued, flush: flushed }));
  }

  function ingestAlarm(frame, context = {}) {
    const event = safety.pushAlarm(frame, context);
    if (!event) return { accepted: false, reason: 'invalid-alarm' };
    const result = alarmRuntime.ingest(event, context);
    if (context.source === 'ble') publishGuardianEvent({ ...event, eventKey: `${event.deviceId}/${event.boot}/${event.id}` });
    return result;
  }

  const guardianMonitor = createGuardianMonitor({
    api: guardianApi,
    pollIntervalMs: guardianConfig.pollIntervalMs,
    offlineAfterMs: guardianConfig.offlineAfterMs,
    eventPageSize: guardianConfig.eventPageSize,
    getQueryContext: async () => {
      const local = state.guardian.binding;
      const confirmed = await guardianApi.getBinding();
      if (state.guardian.role !== ROLES.GUARDIAN || confirmed.role !== ROLES.GUARDIAN
        || !local || !confirmed.binding || local.bindingId !== confirmed.binding.bindingId
        || local !== state.guardian.binding) throw new Error('监护查询绑定已变化，请刷新绑定');
      return { binding: local, bindingId: confirmed.binding.bindingId,
        deviceId: String(confirmed.binding.wearableId || confirmed.binding.deviceId || '').trim() };
    },
    onEvent: (event, queryContext) => {
      const binding = state.guardian.binding;
      const eventDeviceId = String(event && (event.deviceId || event.wearableId) || '').trim();
      const boundDeviceId = String(binding && (binding.wearableId || binding.deviceId) || '').trim();
      const bindingId = String(binding && binding.bindingId || '').trim();
      // listEvents 已由认证后端按当前账号绑定查询；客户端再校验设备，eventPublic 本身没有 bindingId。
      if (!queryContext || queryContext.binding !== binding || queryContext.bindingId !== bindingId
        || queryContext.deviceId !== boundDeviceId
        || state.guardian.role !== ROLES.GUARDIAN || !bindingId || !boundDeviceId || eventDeviceId !== boundDeviceId
        || (event.wearableId && event.wearableId !== eventDeviceId)
        || (event.eventKey && event.eventKey !== `${eventDeviceId}/${event.boot}/${event.id}`)) return;
      return ingestAlarm(event, {
        source: 'guardian',
        origin: 'cloud',
        deviceId: eventDeviceId,
        deviceConfirmed: false,
        cloudSnapshotToken,
        cloudConfirmed: event.deviceConfirmed === true
      });
    },
    onStatus: (status) => updateGuardian({ online: status.online, lastSeenAt: status.lastSeenAt, statusText: status.statusText, lastError: '' }),
    onError: recordGuardianError
  });
  const parser = createProtocolParser({
    onFrame: handleFrame,
    onRawFrame: (raw) => appendDataLog(raw),
    onError: (error) => {
      const message = `协议解析错误：${error.message}`;
      // 单个遥测帧损坏不能把仍在线的 BLE 链路显示成连接错误。
      appendLog('warning', message);
    }
  });
  const simulatorFactory = options.simulatorFactory || createSimulator;
  const simulator = options.simulator || simulatorFactory({
    onData: (arrayBuffer) => parser.push(arrayBuffer),
    onStateChange: (next) => {
      const patch = { simulator: next };
      // 模拟器只能标识数据源，不能伪造 BLE 已连接；没有真实链路活动时，
      // 以可读状态提示当前正式页面的数据来自开发测试源。
      if (!state.connected && !state.connecting && !state.reconnecting && !state.discovering) {
        patch.statusText = next.running
          ? '虚拟手套连续数据流运行中'
          : next.active ? '虚拟手套已注入一帧测试数据'
            : 'Bluetooth disconnected';
      }
      emit(patch);
    }
  });
  function snapshot() {
    return {
      ...state,
      devices: state.devices.slice(),
      rawFlex: state.rawFlex.slice(),
      flex: state.flex.slice(),
      fingerStates: state.fingerStates.slice(),
      rawPose: { ...state.rawPose },
      pose: { ...state.pose },
      acc: { ...state.acc },
      accHealth: { ...state.accHealth },
      bringup: state.bringup ? { ...state.bringup } : null,
      jyStatus: { ...state.jyStatus },
      safetyAlert: state.safetyAlert ? { ...state.safetyAlert } : null,
      care: { ...state.care },
      ppg: { ...state.ppg },
      calibration: { ...state.calibration, enabledFingers: state.calibration.enabledFingers.slice() },
      simulator: {
        ...state.simulator,
        snapshot: state.simulator && state.simulator.snapshot ? {
          flex: state.simulator.snapshot.flex.slice(),
          imu: { ...state.simulator.snapshot.imu },
          acc: { ...state.simulator.snapshot.acc }
        } : null
      },
      alarm: {
        active: state.alarm.active.map((item) => ({ ...item, sources: item.sources ? item.sources.slice() : [] })),
        allActive: (state.alarm.allActive || state.alarm.active).map((item) => ({ ...item, sources: item.sources ? item.sources.slice() : [] })),
        hiddenActiveCount: Number(state.alarm.hiddenActiveCount) || 0,
        history: state.alarm.history.map((item) => ({ ...item, sources: item.sources ? item.sources.slice() : [] })),
        latest: state.alarm.latest ? { ...state.alarm.latest, sources: state.alarm.latest.sources ? state.alarm.latest.sources.slice() : [] } : null
      },
      alarmAckStatus: { ...state.alarmAckStatus },
      guardian: {
        ...state.guardian,
        binding: state.guardian.binding ? { ...state.guardian.binding } : null,
        invite: state.guardian.invite ? { ...state.guardian.invite } : null,
        outbox: state.guardian.outbox ? {
          ...state.guardian.outbox,
          items: Array.isArray(state.guardian.outbox.items) ? state.guardian.outbox.items.map((item) => ({ ...item })) : []
        } : null
      },
      recognition: { ...state.recognition },
      bleLogs: state.bleLogs.slice()
    };
  }

  function notifyListeners() {
    const current = snapshot();
    listeners.forEach((listener) => listener(current));
  }

  function emit(patch = {}) {
    Object.assign(state, patch);
    notifyListeners();
  }

  function emitData(patch = {}) {
    Object.assign(state, patch);
    if (pendingDataTimer) return;
    pendingDataTimer = setTimeout(() => {
      pendingDataTimer = null;
      notifyListeners();
    }, 32);
  }

  function createLogEntry(level, message) {
    return {
      id: `${Date.now()}_${Math.random().toString(16).slice(2)}`,
      time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
      level,
      message: String(message || '')
    };
  }

  function appendLog(level, message) {
    const entry = createLogEntry(level, message);
    emit({ bleLogs: [entry, ...state.bleLogs].slice(0, 80) });
  }

  function appendDataLog(raw) {
    const patch = { lastRawFrame: raw };
    const now = Date.now();
    if (now - lastDataLogAt >= DATA_LOG_INTERVAL_MS) {
      lastDataLogAt = now;
      const entry = createLogEntry('data', raw);
      patch.bleLogs = [entry, ...state.bleLogs].slice(0, 80);
    }
    emitData(patch);
  }

  function flushGuardianOutbox() {
    if (!appForeground) return Promise.resolve({ ok: false, skipped: true, reason: 'background', pending: guardianOutbox ? guardianOutbox.getState().pending : 0 });
    if (!guardianOutbox) return Promise.resolve({ ok: false, skipped: true, reason: 'outbox-unavailable' });
    const eligibility = guardianPublishEligibility();
    if (!eligibility.ok) {
      updateGuardian({ outbox: guardianOutbox.getState() });
      return Promise.resolve({ ok: false, skipped: true, reason: eligibility.reason, pending: guardianOutbox.getState().pending });
    }
    return guardianOutbox.flush({ canSend: (event) => guardianPublishEligibility(event) }).then((result) => {
      updateGuardian({ outbox: guardianOutbox.getState() });
      if (result.failed) {
        updateGuardian({ lastError: `监护事件暂未送达，已保留在本地队列（${result.failed} 次失败）。` });
      }
      return result;
    });
  }

  guardianOutbox = options.guardianOutbox || createGuardianOutbox({
    storage: options.guardianOutboxStorage,
    clock: options.clock,
    maxItems: options.guardianOutboxMaxItems !== undefined ? options.guardianOutboxMaxItems : guardianConfig.outboxMaxItems,
    maxAttemptsPerFlush: options.guardianOutboxMaxAttemptsPerFlush !== undefined ? options.guardianOutboxMaxAttemptsPerFlush : guardianConfig.outboxMaxAttemptsPerFlush,
    baseDelayMs: options.guardianOutboxBaseDelayMs !== undefined ? options.guardianOutboxBaseDelayMs : guardianConfig.outboxBaseDelayMs,
    maxDelayMs: options.guardianOutboxMaxDelayMs !== undefined ? options.guardianOutboxMaxDelayMs : guardianConfig.outboxMaxDelayMs,
    send: sendGuardianOutboxItem,
    onChange: (outbox) => updateGuardian({ outbox })
  });
  state.guardian = { ...state.guardian, outbox: guardianOutbox.getState() };

  function upsertDevice(device) {
    const displayName = String(device.localName || device.name || '').toUpperCase();
    if (!displayName.includes('JDY-23')) return;
    const devices = state.devices.slice();
    const index = devices.findIndex((item) => item.deviceId === device.deviceId);
    if (index >= 0) {
      devices[index] = { ...devices[index], ...device };
    } else {
      devices.push(device);
    }
    devices.sort((left, right) => (right.RSSI || -1000) - (left.RSSI || -1000));
    emit({ devices });
  }

  const alarmAck = createAlarmAckController({
    timeoutMs: options.alarmAckTimeoutMs !== undefined ? options.alarmAckTimeoutMs : guardianConfig.alarmAckTimeoutMs,
    maxAttempts: options.alarmAckMaxAttempts !== undefined ? options.alarmAckMaxAttempts : guardianConfig.alarmAckMaxAttempts,
    retryDelayMs: options.alarmAckRetryDelayMs,
    write: (command) => {
      if (!bluetooth) return Promise.reject(new Error('BLE 写通道不可用'));
      // 报警 ACK 是独立协议单元，必须直接写入 UTF-8 的 LF 结尾；
      // 不能走通用 writeText（它会把结尾规范化为 CRLF）。
      if (typeof bluetooth.write === 'function') return bluetooth.write(encodeUtf8(command));
      if (typeof bluetooth.writeText === 'function') return bluetooth.writeText(command);
      return Promise.reject(new Error('BLE 写通道不可用'));
    },
    getCurrentDeviceId: () => state.deviceId,
    getConnectionToken: () => connectionGeneration,
    onAttempt: ({ attempt, maxAttempts, command, key }) => {
      const displayCommand = command.replace(/\n$/, '');
      appendLog('info', `发送固件报警解除请求（${attempt}/${maxAttempts}）：${displayCommand}`);
      emit({
        alarmAckStatus: {
          ...state.alarmAckStatus,
          stage: 'sending',
          key,
          command,
          attempts: attempt,
          maxAttempts,
          detail: ''
        },
        buzzerStatus: `正在发送设备解除请求（${attempt}/${maxAttempts}）。`
      });
    },
    onWaiting: ({ attempt, maxAttempts, command, key }) => {
      emit({
        alarmAckStatus: {
          ...state.alarmAckStatus,
          stage: 'waiting',
          key,
          command,
          attempts: attempt,
          maxAttempts,
          detail: ''
        },
        buzzerStatus: '请求已写入，等待设备返回匹配的 ACTIVE=0。'
      });
    },
    onResult: (result) => {
      if (result.ok) {
        emit({
          alarmAckStatus: {
            ...state.alarmAckStatus,
            stage: 'resolved',
            key: result.key,
            attempts: result.attempts,
            detail: ''
          },
          buzzerStatus: `设备已解除 ${result.key}：已收到匹配的 ACTIVE=0。`
        });
      } else if (result.reason !== 'stale-device' && result.reason !== 'cancelled') {
        const message = `固件报警解除未完成：${result.detail || result.reason}`;
        emit({
          alarmAckStatus: {
            ...state.alarmAckStatus,
            stage: 'failed',
            key: result.key,
            attempts: result.attempts,
            detail: result.detail || result.reason
          },
          buzzerStatus: `${message} 可重试。`
        });
        appendLog('error', message);
      } else if (state.alarmAckStatus.key === result.key) {
        emit({
          alarmAckStatus: {
            ...state.alarmAckStatus,
            stage: 'failed',
            key: result.key,
            attempts: result.attempts,
            detail: result.reason
          },
          buzzerStatus: `设备解除请求已取消：${result.reason}。`
        });
      }
    }
  });

  function applyFlexCalibration(rawFlex, calibration = state.calibration) {
    return calibrationStore.mapFlex(rawFlex, calibration.flexZero, calibration.flexFull);
  }

  function saveCalibration(patch) {
    const calibration = calibrationStore.write({ ...state.calibration, ...patch });
    const flex = validFlex(state.rawFlex) ? applyFlexCalibration(state.rawFlex, calibration) : state.flex;
    const pose = Number.isFinite(state.rawPose.roll)
      ? calibrationStore.offsetImu(state.rawPose, calibration.imuZero)
      : state.pose;
    emit({ calibration, flex, fingerStates: classifyFingers(flex), pose });
    return calibration;
  }

  function reconcileAlarmState(frame) {
    if (!state.connected || !state.deviceId) {
      appendLog('warning', '已忽略非当前连接设备的报警状态心跳。');
      return;
    }

    const receivedAt = Date.now();
    if (currentDeviceBoot !== null && currentDeviceBoot !== frame.boot) {
      connectionGeneration += 1;
      alarmAck.cancel('stale-device');
    }
    currentDeviceBoot = frame.boot;

    const alarmState = alarmRuntime.getState();
    const activeRecords = (alarmState.allActive || alarmState.active)
      .filter((event) => event.deviceId === state.deviceId);
    activeRecords.forEach((event) => {
      const stillActive = frame.active
        && event.boot === frame.boot
        && event.id === frame.id
        && event.alarmType === frame.alarmType;
      if (stillActive) return;
      const cleared = {
        type: 'alarm',
        boot: event.boot,
        id: event.id,
        alarmType: event.alarmType,
        active: false
      };
      ingestAlarm(cleared, {
        source: 'ble',
        origin: 'device',
        deviceId: state.deviceId,
        deviceConfirmed: true
      });
      alarmAck.notifyResolved({ ...cleared, deviceId: state.deviceId });
    });

    if (frame.active) {
      ingestAlarm({ ...frame, type: 'alarm' }, {
        source: 'ble',
        origin: 'device',
        deviceId: state.deviceId,
        deviceConfirmed: true
      });
    }
    emitData({ lastError: '', lastFrameAt: receivedAt });
  }

  function handleFrame(frame) {
    touchGuardianStatusIfNeeded();
    if (frame.type === 'boot') {
      currentDeviceBoot = null;
      dynamicJYReceived = false;
      connectionGeneration += 1;
      alarmAck.cancel('stale-device');
      // BOOT 只开启新的报警会话；启动 JY 值不能冒充当前动态状态。
      emitData({ bootName: frame.name, jyValid: false, jyStatus: emptyJYStatus(), lastError: '', lastFrameAt: Date.now() });
      return;
    }
    if (frame.type === 'flex') {
      const rawFlex = [...frame.left, ...frame.right];
      const flex = applyFlexCalibration(rawFlex);
      const receivedAt = Date.now();
      // 远控新鲜度只由真实 FLEX 帧更新，ACC/IMU/ALARM/诊断帧不能冒充手部数据。
      emitData({ rawFlex, flex, fingerStates: classifyFingers(flex), lastError: '', lastFrameAt: receivedAt, lastFlexAt: receivedAt });
      return;
    }
    if (frame.type === 'imu') {
      const rawPose = { roll: frame.roll, pitch: frame.pitch, yaw: frame.yaw };
      const pose = calibrationStore.offsetImu(rawPose, state.calibration.imuZero);
      emitData({ rawPose, pose, lastError: '', lastFrameAt: Date.now() });
      return;
    }
    if (frame.type === 'acc') {
      const receivedAt = Date.now();
      emitData({
        acc: { ...frame, lastAt: receivedAt },
        accHealth: { valid: frame.valid === true, lastAt: receivedAt, statusText: frame.valid === true ? 'ACC 有效' : 'ACC 无效（仅诊断，不触发报警）' },
        lastError: '',
        lastFrameAt: receivedAt
      });
      return;
    }
    if (frame.type === 'alarmState') {
      reconcileAlarmState(frame);
      return;
    }
    if (frame.type === 'alarm') {
      if (!state.connected || !state.deviceId) {
        appendLog('warning', '已忽略非当前连接设备的 BLE ALARM。');
        return;
      }
      const receivedAt = Date.now();
      if (frame.active) {
        if (currentDeviceBoot !== null && currentDeviceBoot !== frame.boot) {
          connectionGeneration += 1;
          alarmAck.cancel('stale-device');
        }
        currentDeviceBoot = frame.boot;
      } else if (currentDeviceBoot === null || frame.boot !== currentDeviceBoot) {
        appendLog('warning', '已忽略旧启动会话的 BLE ACTIVE=0。');
        emitData({ lastError: '', lastFrameAt: receivedAt });
        return;
      } else {
        const existing = alarmRuntime.find(`${state.deviceId}/${frame.boot}/${frame.id}`);
        if (!existing || existing.status !== 'active') {
          appendLog('warning', '已忽略未匹配活动事件的 BLE ACTIVE=0。');
          emitData({ lastError: '', lastFrameAt: receivedAt });
          return;
        }
      }
      const result = ingestAlarm(frame, {
        source: 'ble',
        origin: 'device',
        deviceId: state.deviceId,
        deviceConfirmed: true
      });
      if (!frame.active) {
        // 先通知 ACK 等待器；运行时仍以同一条当前设备 ACTIVE=0 作为权威解除。
        alarmAck.notifyResolved({ ...frame, deviceId: state.deviceId });
      }
      if (!result.accepted && result.reason === 'invalid-alarm') {
        appendLog('warning', '已忽略格式无效的 BLE ALARM。');
      }
      emitData({ lastError: '', lastFrameAt: receivedAt });
      return;
    }
    if (frame.type === 'jy') {
      const sampleAgeMs = frame.jySampleAgeMs;
      const stale = sampleAgeMs > JY_MAX_SAMPLE_AGE_MS;
      const online = frame.jyOnline === true;
      const receivedAt = Date.now();
      const jyStatus = {
        source: 'dynamic',
        online,
        errorStreak: frame.jyErrorStreak,
        lastErrorMask: frame.jyLastError,
        sampleAgeMs,
        stale,
        currentSampleValid: frame.jyLastError === JY_CURRENT_SAMPLE_OK,
        receivedAt
      };
      dynamicJYReceived = true;
      // LAST=16 表示固件保留了上一次姿态用于显示，但本次读取不是
      // 新鲜有效样本。所有非零 LAST 都暂停客户端安全判定，避免把缓存
      // IMU 或部分 I2C 失败当作真实运动。
      const jyValid = online && !stale && frame.jyLastError === JY_CURRENT_SAMPLE_OK;
      safety.setJYStatus(jyValid);
      emitData({ jyStatus, jyValid, lastError: '', lastFrameAt: receivedAt });
      return;
    }
    if (frame.type === 'bringup') {
      let jyValid = null;
      if (typeof frame.jyValid === 'boolean') jyValid = frame.jyValid;
      else if (typeof frame.jy === 'number' && typeof frame.jyRet === 'number') jyValid = frame.jy === 1 && frame.jyRet === 0;
      else if ([frame.jyRight, frame.jyLeft, frame.jyRightRet, frame.jyLeftRet].every((value) => typeof value === 'number')) {
        jyValid = frame.jyRight === 1 && frame.jyLeft === 1 && frame.jyRightRet === 0 && frame.jyLeftRet === 0;
      }
      if (jyValid !== null && !dynamicJYReceived) safety.setJYStatus(jyValid);
      const patch = { bringup: frame, lastError: '', lastFrameAt: Date.now() };
      if (jyValid !== null && !dynamicJYReceived) patch.jyValid = jyValid;
      emitData(patch);
      return;
    }
    if (frame.type === 'care') {
      // CARE 只作为演示数据通道展示，不进入真实报警事件或监护上报。
      emitData({ care: { ...frame, source: 'demo' }, lastError: '', lastFrameAt: Date.now() });
      return;
    }
    if (frame.type === 'ppg') {
      emitData({ ppg: { ...frame }, lastError: '', lastFrameAt: Date.now() });
    }
  }

  const bluetoothFactory = options.bluetoothFactory || createBluetoothClient;
  bluetooth = options.bluetooth || bluetoothFactory({
    onStateChange: (next) => {
      const nextDeviceId = typeof next.deviceId === 'string' ? next.deviceId : state.deviceId;
      const deviceChanged = nextDeviceId !== lastDeviceId;
      const disconnected = next.connected === false;
      if (deviceChanged || disconnected) {
        lastDeviceId = nextDeviceId;
        currentDeviceBoot = null;
        dynamicJYReceived = false;
        connectionGeneration += 1;
        alarmAck.cancel(deviceChanged ? 'stale-device' : 'disconnected');
      }
      const statePatch = {
        adapterReady: next.adapterReady !== false,
        discovering: typeof next.discovering === 'boolean' ? next.discovering : state.discovering,
        connecting: typeof next.connecting === 'boolean' ? next.connecting : state.connecting,
        connected: typeof next.connected === 'boolean' ? next.connected : state.connected,
        reconnecting: typeof next.reconnecting === 'boolean' ? next.reconnecting : state.reconnecting,
        deviceName: typeof next.deviceName === 'string' ? next.deviceName : state.deviceName,
        deviceId: nextDeviceId,
        serviceId: typeof next.serviceId === 'string' ? next.serviceId : state.serviceId,
        characteristicId: typeof next.characteristicId === 'string' ? next.characteristicId : state.characteristicId,
        notifyEnabled: typeof next.notifyEnabled === 'boolean' ? next.notifyEnabled : state.notifyEnabled,
        statusText: next.message || state.statusText
      };
      if (deviceChanged || disconnected) {
        statePatch.jyValid = false;
        statePatch.jyStatus = emptyJYStatus();
      }
      emit(statePatch);
      touchGuardianStatusIfNeeded();
    },
    onDeviceFound: upsertDevice,
    onValueChange: (arrayBuffer, metadata = {}) => {
      if (metadata.deviceId && metadata.deviceId !== state.deviceId) return;
      parser.push(arrayBuffer);
    },
    onError: (error) => {
      const message = `蓝牙错误：${error.errMsg || error.message || error}`;
      emit({ lastError: message });
      appendLog('error', message);
    }
  });

  return {
    getState: snapshot,
    subscribe(listener) {
      listeners.add(listener);
      listener(snapshot());
      return () => listeners.delete(listener);
    },
    setMode(mode) {
      if (!Object.values(MODES).includes(mode)) throw new Error(`Unknown mode: ${mode}`);
      emit({ mode });
    },
    async scan() {
      emit({ devices: [], lastError: '' });
      appendLog('info', '开始扫描 JDY-23。');
      try {
        await bluetooth.startDiscovery();
      } catch (error) {
        const message = `蓝牙扫描失败：${error.errMsg || error.message || error}`;
        emit({ lastError: message, statusText: message });
        appendLog('error', message);
        throw error;
      }
    },
    async connect(device) {
      parser.reset();
      currentDeviceBoot = null;
      appendLog('info', `请求连接 ${device && (device.localName || device.name) || 'JDY-23'}。`);
      try {
        await bluetooth.connect(device);
        appendLog('info', 'FFE0/FFE1 已连接，Notify 已开启。');
      } catch (error) {
        const message = error.errMsg || error.message || error;
        emit({ lastError: String(message) });
        appendLog('error', message);
        throw error;
      }
    },
    disconnect() {
      parser.reset();
      currentDeviceBoot = null;
      alarmAck.cancel('disconnected');
      return bluetooth.disconnect();
    },
    close() {
      parser.reset();
      guardianMonitor.stop();
      alarmAck.cancel('cancelled');
      alarmAudio.stop();
      return bluetooth.closeAdapter();
    },
    writeText(text) {
      appendLog('info', `发送 BLE 命令：${text}`);
      return bluetooth.writeText(text).catch((error) => {
        appendLog('error', `BLE 写入失败：${error.errMsg || error.message || error}`);
        throw error;
      });
    },
    acknowledgeAlarm(eventKey, actor = 'local') {
      const result = alarmRuntime.acknowledge(eventKey, actor);
      if (!result.accepted) return Promise.resolve(result);
      const event = result.event;
      const boundDeviceId = String(state.guardian.binding && (state.guardian.binding.wearableId || state.guardian.binding.deviceId) || '').trim();
      if (state.guardian.binding && guardianApi.isConfigured() && event && boundDeviceId === event.deviceId) {
        return guardianApi.acknowledgeEvent(eventKey).then(() => result).catch((error) => {
          recordGuardianError(error);
          return result;
        });
      }
      return Promise.resolve(result);
    },
    hideLocalAlarm(eventKey) {
      if (state.guardian.role === ROLES.GUARDIAN) return Promise.resolve({ accepted: false, reason: 'remote-hide-forbidden' });
      if (state.connected) return Promise.resolve({ accepted: false, reason: 'device-connected' });
      const result = alarmRuntime.hideLocally(eventKey);
      if (result.accepted) {
        alarmAudio.stop();
        emit({ alarmAudioStatus: '本机旧提示已隐藏；设备未连接，尚未核验是否解除。重新连接后若仍活动会再次显示。' });
      }
      return Promise.resolve(result);
    },
    requestAlarmResolve(eventKey) {
      if (state.guardian.role === ROLES.GUARDIAN) return Promise.resolve({ accepted: false, reason: 'remote-resolve-forbidden' });
      const event = alarmRuntime.find(eventKey);
      if (!event) return Promise.resolve({ accepted: false, reason: 'not-found' });
      if (event.status !== 'active' || event.active !== true) return Promise.resolve({ accepted: false, reason: 'not-active', event });
      if (!state.connected || !state.deviceId) return Promise.resolve({ accepted: false, reason: 'device-not-connected', event });
      if (event.deviceId !== state.deviceId) return Promise.resolve({ accepted: false, reason: 'stale-device', event });
      if (currentDeviceBoot === null || event.boot !== currentDeviceBoot) return Promise.resolve({ accepted: false, reason: 'stale-device', event });
      if (alarmAck.getState().keys.includes(String(eventKey))) {
        return Promise.resolve({ accepted: false, reason: 'already-pending', event });
      }
      const requested = alarmRuntime.requestResolve(eventKey, ROLES.WEARER);
      if (!requested.accepted) return Promise.resolve(requested);
      return alarmAck.request(event, { deviceId: state.deviceId, connectionToken: connectionGeneration }).then((result) => {
        if (!result.ok) return { accepted: false, reason: result.reason, detail: result.detail, event: alarmRuntime.find(eventKey) || event };
        return { accepted: true, action: 'resolved-by-device', event: alarmRuntime.find(eventKey) || event };
      });
    },
    acknowledgeSafety() {
      const event = state.safetyAlert;
      if (!event) return Promise.resolve({ accepted: false, reason: 'not-active' });
      return this.acknowledgeAlarm(event.eventKey, 'local');
    },
    getAlarmRuntime() {
      return alarmRuntime;
    },
    setGuardianRole(role) {
      const next = guardianBinding.selectRole(role);
      if (next.role !== ROLES.GUARDIAN) guardianMonitor.stop();
      updateGuardian({ ...next, lastError: '', statusText: next.configured ? '角色已保存，可刷新绑定状态。' : '监护服务未配置：请填写环境 ID 并部署云函数。' });
      return next;
    },
    refreshGuardian() {
      if (!guardianApi.isConfigured()) {
        const result = { ...guardianBinding.getState(), configured: false };
        guardianMonitor.stop();
        updateGuardian({ ...result, outbox: guardianOutbox.getState(), lastError: '', statusText: '监护服务未配置：请填写环境 ID 并部署云函数。' });
        return Promise.resolve(result);
      }
      return guardianBinding.refresh().then((next) => {
        if (!(next.role === ROLES.GUARDIAN && next.binding)) guardianMonitor.stop();
        updateGuardian({ ...next, outbox: guardianOutbox.getState(), lastError: '', statusText: next.binding ? '绑定状态已更新。' : '尚未完成绑定。' });
        return next;
      }).catch((error) => { recordGuardianError(error); throw error; });
    },
    createGuardianInvite(ttlMs) {
      return guardianBinding.createInvite(state.deviceId, ttlMs).then((next) => {
        updateGuardian({ ...next, lastError: '', statusText: '邀请码已生成，请在有效期内交给监护者。' });
        return next;
      }).catch((error) => { recordGuardianError(error); throw error; });
    },
    acceptGuardianInvite(code) {
      return guardianBinding.acceptInvite(code).then((next) => {
        updateGuardian({ ...next, lastError: '', statusText: '绑定已完成，可开始接收报警。' });
        return next;
      }).catch((error) => { recordGuardianError(error); throw error; });
    },
    startGuardianMonitor() {
      if (state.guardian.role !== ROLES.GUARDIAN) return Promise.resolve({ started: false, reason: 'role-not-guardian' });
      if (!appForeground) return Promise.resolve({ started: false, reason: 'background' });
      if (!guardianApi.isConfigured()) {
        updateGuardian({ statusText: '监护服务未配置：请填写环境 ID 并部署云函数。', lastError: '' });
        return Promise.resolve({ started: false, reason: 'not-configured' });
      }
      updateGuardian({ statusText: '正在轮询监护事件…', lastError: '' });
      return guardianMonitor.start().then(() => ({ started: true }));
    },
    stopGuardianMonitor() {
      guardianMonitor.stop();
    },
    pollGuardian() {
      return guardianMonitor.poll();
    },
    flushGuardianOutbox,
    getGuardianOutbox() {
      return guardianOutbox;
    },
    onAppShow() {
      appForeground = true;
      updateGuardian({ foreground: true });
      return this.refreshGuardian().then((next) => {
        if (next.role === ROLES.GUARDIAN && next.binding) return this.startGuardianMonitor().then(() => next);
        guardianMonitor.stop();
        return next;
      }).then((next) => flushGuardianOutbox().then(() => next)).catch((error) => {
        guardianMonitor.stop();
        throw error;
      });
    },
    onAppHide() {
      appForeground = false;
      guardianMonitor.stop();
      updateGuardian({ foreground: false });
    },
    getGuardianApi() {
      return guardianApi;
    },
    getGuardianBinding() {
      return guardianBinding;
    },
    getGuardianMonitor() {
      return guardianMonitor;
    },
    ingestRawData(arrayBuffer) {
      parser.push(arrayBuffer);
    },
    getSimulator() {
      return simulator;
    },
    startSimulator() {
      return simulator.start();
    },
    stopSimulator() {
      return simulator.stop();
    },
    injectSimulator(simulatorOptions) {
      return simulator.inject(simulatorOptions);
    },
    setSimulatorSnapshot(next, simulatorOptions) {
      return simulator.setSnapshot(next, simulatorOptions);
    },
    applySimulatorPreset(name, simulatorOptions) {
      return simulator.applyPreset(name, simulatorOptions);
    },
    loadSimulatorCase(testCase, simulatorOptions) {
      return simulator.loadCase(testCase, simulatorOptions);
    },
    getAlarmAckController() {
      return alarmAck;
    },
    setRecognition(next = {}) {
      const recognition = {
        name: String(next.name || ''),
        text: String(next.text || '等待手势'),
        status: String(next.status || '')
      };
      const previous = state.recognition;
      if (previous.name === recognition.name && previous.text === recognition.text && previous.status === recognition.status) return;
      emit({ recognition });
    },
    captureFlexZero() {
      if (!validFlex(state.rawFlex)) throw new Error('当前没有完整十指数据，无法置零。');
      saveCalibration({ flexZero: state.rawFlex.slice() });
      appendLog('info', '已保存当前十指 FLEX Zero。');
    },
    captureFlexFull() {
      if (!validFlex(state.rawFlex)) throw new Error('当前没有完整十指数据，无法握满校准。');
      saveCalibration({ flexFull: state.rawFlex.slice() });
      appendLog('info', '已保存当前十指 FLEX Full。');
    },
    captureImuZero() {
      const pose = state.rawPose;
      if (![pose.roll, pose.pitch, pose.yaw].every((value) => Number.isFinite(value))) throw new Error('当前没有完整 IMU 数据，无法置零。');
      saveCalibration({ imuZero: { ...pose } });
      appendLog('info', '已保存当前 IMU Zero。');
    },
    toggleFinger(index) {
      if (!Number.isInteger(index) || index < 0 || index >= 10) throw new Error('手指索引无效。');
      const enabledFingers = state.calibration.enabledFingers.slice();
      enabledFingers[index] = !enabledFingers[index];
      saveCalibration({ enabledFingers });
    },
    getBluetoothClient() {
      return bluetooth;
    },
    modes: MODES
  };
}

module.exports = { MODES, createAppState };
