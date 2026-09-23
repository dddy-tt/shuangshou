const DEFAULT_OUTBOX_KEY = 'shuangshou.guardian-outbox.v1';
const DEFAULT_MAX_ITEMS = 50;
const DEFAULT_MAX_ATTEMPTS_PER_FLUSH = 8;
const DEFAULT_BASE_DELAY_MS = 2000;
const DEFAULT_MAX_DELAY_MS = 60000;

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function defaultStorage() {
  return {
    read(key) {
      if (typeof wx === 'undefined' || typeof wx.getStorageSync !== 'function') return null;
      return wx.getStorageSync(key);
    },
    write(key, value) {
      if (typeof wx !== 'undefined' && typeof wx.setStorageSync === 'function') wx.setStorageSync(key, value);
    }
  };
}

function detailOf(error) {
  return error && (error.errMsg || error.message || error.msg) || String(error || '监护上报失败');
}

function boolActive(value) {
  if (typeof value === 'boolean') return value;
  if (value === 1 || value === '1') return true;
  if (value === 0 || value === '0') return false;
  throw new Error('监护事件 ACTIVE 必须是布尔值或 0/1');
}

function normalizeEvent(input = {}) {
  const deviceId = String(input.deviceId || input.wearableId || '').trim();
  if (input.deviceId !== undefined && input.wearableId !== undefined
    && String(input.deviceId).trim() !== String(input.wearableId).trim()) {
    throw new Error('监护事件 deviceId 与 wearableId 不一致');
  }
  const boot = Number(input.boot !== undefined ? input.boot : input.BOOT);
  const id = Number(input.id !== undefined ? input.id : input.ID);
  const alarmType = Number(input.alarmType !== undefined
    ? input.alarmType
    : input.typeCode !== undefined ? input.typeCode : input.TYPE !== undefined ? input.TYPE : input.type);
  const active = boolActive(input.active !== undefined ? input.active : input.ACTIVE);
  if (!deviceId) throw new Error('监护事件缺少 deviceId');
  if (![boot, id].every(Number.isInteger) || boot < 0 || boot > 0xFFFFFFFF || id < 0 || id > 0xFFFFFFFF) {
    throw new Error('监护事件 BOOT/ID 必须是 uint32');
  }
  if (![1, 2].includes(alarmType)) throw new Error('监护事件 TYPE 不受支持');
  const eventKey = `${deviceId}/${boot}/${id}`;
  if (input.eventKey && String(input.eventKey) !== eventKey) throw new Error('监护事件 eventKey 与设备/BOOT/ID 不一致');
  if (input.source && !['ble', 'device'].includes(String(input.source))) throw new Error('监护队列只接受当前设备真实来源');
  if (input.origin && input.origin !== 'device') throw new Error('只有设备真实报警允许进入监护上报队列');
  return {
    eventKey,
    wearableId: deviceId,
    deviceId,
    bindingId: String(input.bindingId || '').trim(),
    boot,
    id,
    alarmType,
    active,
    origin: 'device',
    timestamp: Number.isFinite(Number(input.timestamp !== undefined ? input.timestamp : input.occurredAt))
      ? Number(input.timestamp !== undefined ? input.timestamp : input.occurredAt)
      : Date.now()
  };
}

function createGuardianOutbox(options = {}) {
  const storage = options.storage || defaultStorage();
  const storageKey = options.storageKey || DEFAULT_OUTBOX_KEY;
  const clock = options.clock || (() => Date.now());
  const maxItems = Math.max(1, Number(options.maxItems) || DEFAULT_MAX_ITEMS);
  const maxAttemptsPerFlush = Math.max(1, Number(options.maxAttemptsPerFlush) || DEFAULT_MAX_ATTEMPTS_PER_FLUSH);
  const configuredBaseDelay = Number(options.baseDelayMs);
  const configuredMaxDelay = Number(options.maxDelayMs);
  const baseDelayMs = Math.max(0, Number.isFinite(configuredBaseDelay) ? configuredBaseDelay : DEFAULT_BASE_DELAY_MS);
  const maxDelayMs = Math.max(baseDelayMs, Number.isFinite(configuredMaxDelay) ? configuredMaxDelay : DEFAULT_MAX_DELAY_MS);
  const send = options.send;
  const onChange = options.onChange || function noop() {};
  let flushPromise = null;
  let state = {
    version: 1,
    nextSequence: 1,
    items: [],
    sent: {},
    lastError: '',
    lastFlushAt: 0
  };

  function now() {
    const value = Number(clock());
    return Number.isFinite(value) ? value : Date.now();
  }

  function readStored() {
    try { return storage.read(storageKey); } catch (error) { return null; }
  }

  function restore() {
    const stored = readStored();
    if (!stored) return;
    const source = Array.isArray(stored) ? { items: stored } : stored;
    if (!source || !Array.isArray(source.items)) return;
    state.items = source.items.map((item) => {
      if (!item || !item.eventKey || !['active', 'resolve'].includes(item.phase)) return null;
      const wearableId = String(item.wearableId || item.deviceId || '').trim();
      const deviceId = String(item.deviceId || item.wearableId || '').trim();
      const boot = Number(item.boot);
      const id = Number(item.id);
      const alarmType = Number(item.alarmType);
      const expectedKey = `${deviceId}/${boot}/${id}`;
      if (!deviceId || wearableId !== deviceId || String(item.eventKey) !== expectedKey
        || ![boot, id].every(Number.isInteger)
        || boot < 0 || boot > 0xFFFFFFFF || id < 0 || id > 0xFFFFFFFF
        || ![1, 2].includes(alarmType)) return null;
      return {
        eventKey: expectedKey,
        wearableId,
        deviceId,
        bindingId: String(item.bindingId || '').trim(),
        boot,
        id,
        alarmType,
        active: item.phase === 'active',
        origin: 'device',
        timestamp: Number(item.timestamp) || 0,
        phase: item.phase,
        synthetic: item.synthetic === true,
        sequence: Number(item.sequence) || 0,
        attempts: Math.max(0, Number(item.attempts) || 0),
        nextAttemptAt: Math.max(0, Number(item.nextAttemptAt) || 0),
        lastError: String(item.lastError || '')
      };
    }).filter((item) => item && !(item.phase === 'active' && item.synthetic === true)).slice(0, maxItems);
    state.nextSequence = Math.max(
      Number(source.nextSequence) || 1,
      ...state.items.map((item) => item.sequence + 1),
      1
    );
    state.sent = {};
    if (source.sent && typeof source.sent === 'object') {
      Object.entries(source.sent).forEach(([eventKey, item]) => {
        if (!item || typeof item !== 'object') return;
        const parts = String(eventKey).split('/');
        if (parts.length < 3) return;
        const deviceId = parts.slice(0, -2).join('/');
        const boot = Number(parts[parts.length - 2]);
        const id = Number(parts[parts.length - 1]);
        if (!deviceId || ![boot, id].every(Number.isInteger)
          || boot < 0 || boot > 0xFFFFFFFF || id < 0 || id > 0xFFFFFFFF) return;
        state.sent[`${deviceId}/${boot}/${id}`] = { ...item };
      });
    }
    state.lastError = String(source.lastError || '');
    state.lastFlushAt = Number(source.lastFlushAt) || 0;
  }

  restore();

  function persist() {
    try {
      storage.write(storageKey, clone(state));
    } catch (error) {
      // 本地存储失败不能让事件直接丢失；内存队列仍保留并在下一次前台机会重试。
      state.lastError = `监护队列保存失败：${detailOf(error)}`;
    }
  }

  function snapshot() {
    const pending = state.items.length;
    const nextRetryAt = state.items.reduce((minimum, item) => {
      if (!item.nextAttemptAt) return minimum;
      return minimum === 0 ? item.nextAttemptAt : Math.min(minimum, item.nextAttemptAt);
    }, 0);
    return {
      pending,
      items: clone(state.items),
      lastError: state.lastError,
      lastFlushAt: state.lastFlushAt,
      nextRetryAt,
      maxItems
    };
  }

  function changed() {
    persist();
    try { onChange(snapshot()); } catch (error) { /* UI 状态回调不能阻断队列 */ }
  }

  function sentState(eventKey) {
    return state.sent[eventKey] || { active: false, resolve: false, lastSentAt: 0, alarmType: null, bindingId: '' };
  }

  function hasItem(eventKey, phase) {
    return state.items.some((item) => item.eventKey === eventKey && item.phase === phase);
  }

  function makeItem(event, phase, synthetic = false) {
    const item = {
      ...event,
      phase,
      active: phase === 'active',
      synthetic,
      sequence: state.nextSequence++,
      attempts: 0,
      nextAttemptAt: 0,
      lastError: ''
    };
    return item;
  }

  function enqueue(input) {
    let event;
    try { event = normalizeEvent(input); } catch (error) {
      return { accepted: false, reason: 'invalid-event', detail: detailOf(error) };
    }
    const sent = sentState(event.eventKey);
    const queuedType = state.items.find((item) => item.eventKey === event.eventKey);
    const knownType = queuedType ? queuedType.alarmType : sent.alarmType;
    if (knownType !== null && knownType !== undefined && Number(knownType) !== event.alarmType) {
      return { accepted: false, reason: 'event-type-mismatch', eventKey: event.eventKey, pending: state.items.length };
    }
    const knownBindingId = queuedType ? queuedType.bindingId : sent.bindingId;
    if (knownBindingId && event.bindingId && knownBindingId !== event.bindingId) {
      return { accepted: false, reason: 'binding-mismatch', eventKey: event.eventKey, pending: state.items.length };
    }
    const phase = event.active ? 'active' : 'resolve';
    if (sent.resolve || sent[phase] || hasItem(event.eventKey, phase)
      || (phase === 'active' && hasItem(event.eventKey, 'resolve'))) {
      return { accepted: false, reason: 'deduplicated', eventKey: event.eventKey, phase, pending: state.items.length };
    }

    // 服务端允许首次直接创建 RESOLVE；只有队列中已有真实 ACTIVE 时才保持顺序。
    const additions = [makeItem(event, phase)];
    if (state.items.length + additions.length > maxItems) {
      return { accepted: false, reason: 'outbox-full', eventKey: event.eventKey, phase, pending: state.items.length };
    }
    state.items.push(...additions);
    state.items.sort((left, right) => left.sequence - right.sequence);
    state.lastError = '';
    changed();
    return { accepted: true, action: 'enqueued', eventKey: event.eventKey, phase, pending: state.items.length };
  }

  function eligible(optionsForFlush, item) {
    if (typeof optionsForFlush.canSend !== 'function') return { ok: true };
    let result;
    try { result = optionsForFlush.canSend(item); } catch (error) { return { ok: false, reason: 'eligibility-error', detail: detailOf(error) }; }
    if (result === true || result === undefined) return { ok: true };
    if (result && typeof result === 'object') return { ok: result.ok === true, reason: result.reason || 'not-eligible', detail: result.detail };
    return { ok: false, reason: 'not-eligible' };
  }

  function readyItem(timestamp) {
    const sorted = state.items.slice().sort((left, right) => left.sequence - right.sequence);
    return sorted.find((item) => {
      if (item.nextAttemptAt > timestamp) return false;
      if (item.phase === 'active') return true;
      const activePending = state.items.some((candidate) => candidate.eventKey === item.eventKey && candidate.phase === 'active');
      return !activePending;
    }) || null;
  }

  function payloadFor(item) {
    return {
      eventKey: item.eventKey,
      wearableId: item.wearableId,
      deviceId: item.deviceId,
      boot: item.boot,
      id: item.id,
      alarmType: item.alarmType,
      active: item.phase === 'active',
      origin: 'device',
      timestamp: item.timestamp,
      synthetic: item.synthetic === true
    };
  }

  function backoff(attempts) {
    if (!baseDelayMs) return 0;
    return Math.min(maxDelayMs, baseDelayMs * (2 ** Math.max(0, attempts - 1)));
  }

  function markSent(item) {
    const previous = sentState(item.eventKey);
    state.sent[item.eventKey] = {
      active: previous.active === true || item.phase === 'active',
      resolve: previous.resolve === true || item.phase === 'resolve',
      lastSentAt: now(),
      sequence: item.sequence,
      alarmType: item.alarmType,
      bindingId: item.bindingId || previous.bindingId || ''
    };
    state.items = state.items.filter((candidate) => candidate !== item);
    // 发送去重记录只保留有限窗口，避免本地元数据无限增长。
    const sentKeys = Object.keys(state.sent);
    const keepCount = maxItems * 4;
    if (sentKeys.length > keepCount) {
      sentKeys.sort((left, right) => (state.sent[left].sequence || 0) - (state.sent[right].sequence || 0));
      sentKeys.slice(0, sentKeys.length - keepCount).forEach((key) => delete state.sent[key]);
    }
  }

  async function runFlush(optionsForFlush = {}) {
    let sentCount = 0;
    let failedCount = 0;
    for (let work = 0; work < maxAttemptsPerFlush; work += 1) {
      const item = readyItem(now());
      if (!item) break;
      const itemEligibility = eligible(optionsForFlush, item);
      if (!itemEligibility.ok) {
        return { ok: sentCount > 0 && failedCount === 0, sent: sentCount, failed: failedCount, skipped: true, reason: itemEligibility.reason, pending: state.items.length };
      }
      try {
        if (typeof send !== 'function') throw new Error('监护发送函数不可用');
        const result = await send(payloadFor(item), item);
        if (result && result.ok === false) throw new Error(result.detail || result.reason || '监护服务拒绝事件');
        markSent(item);
        state.lastError = '';
        sentCount += 1;
        changed();
      } catch (error) {
        item.attempts += 1;
        item.lastError = detailOf(error);
        item.nextAttemptAt = now() + backoff(item.attempts);
        state.lastError = item.lastError;
        failedCount += 1;
        changed();
        break;
      }
    }
    state.lastFlushAt = now();
    if (!failedCount && !state.items.length) state.lastError = '';
    changed();
    return { ok: failedCount === 0, sent: sentCount, failed: failedCount, pending: state.items.length };
  }

  function flush(optionsForFlush = {}) {
    if (flushPromise) return flushPromise;
    const running = runFlush(optionsForFlush);
    const wrapped = running.finally(() => {
      if (flushPromise === wrapped) flushPromise = null;
    });
    flushPromise = wrapped;
    return wrapped;
  }

  return {
    clear() {
      state.items = [];
      state.lastError = '';
      changed();
    },
    enqueue,
    flush,
    getState: snapshot,
    key: storageKey
  };
}

module.exports = {
  DEFAULT_BASE_DELAY_MS,
  DEFAULT_MAX_DELAY_MS,
  DEFAULT_MAX_ITEMS,
  DEFAULT_OUTBOX_KEY,
  createGuardianOutbox,
  normalizeEvent
};
