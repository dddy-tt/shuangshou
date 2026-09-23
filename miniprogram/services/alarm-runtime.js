const ALARM_LABELS = Object.freeze({
  1: '疑似跌倒',
  2: '异常抖动'
});

const DEFAULT_HISTORY_KEY = 'shuangshou.alarm-history.v1';

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function nowValue(clock) {
  const value = Number(clock());
  return Number.isFinite(value) ? value : Date.now();
}

function normalizeActive(value) {
  if (typeof value === 'boolean') return value;
  if (value === 1 || value === '1') return true;
  if (value === 0 || value === '0') return false;
  throw new Error('ALARM ACTIVE 必须是布尔值或 0/1');
}

function normalizeAlarmEvent(input, options = {}) {
  const value = input || {};
  const boot = Number(value.boot !== undefined ? value.boot : value.BOOT);
  const id = Number(value.id !== undefined ? value.id : value.ID);
  const alarmType = Number(
    value.alarmType !== undefined
      ? value.alarmType
      : value.typeCode !== undefined
        ? value.typeCode
        : typeof value.type === 'number' ? value.type : NaN
  );
  if (![boot, id].every(Number.isInteger) || boot < 0 || boot > 0xFFFFFFFF || id < 0 || id > 0xFFFFFFFF) {
    throw new Error('报警事件 BOOT/ID 必须是 uint32');
  }
  if (![1, 2].includes(alarmType)) throw new Error('报警事件 TYPE 不受支持');
  const active = normalizeActive(value.active !== undefined ? value.active : value.ACTIVE);
  const deviceId = String(value.deviceId || value.wearableId || options.deviceId || '').trim();
  if (!deviceId) throw new Error('报警事件缺少 deviceId');
  const source = String(value.source || options.source || 'unknown');
  const origin = String(value.origin || options.origin || (source === 'ble' ? 'device' : 'cloud'));
  const timestamp = Number(value.timestamp !== undefined ? value.timestamp : value.occurredAt);
  const eventKey = `${deviceId}/${boot}/${id}`;
  const deviceConfirmed = source === 'ble' || (source === 'device' && value.deviceConfirmed === true);
  // 这是经过 app-state 当前绑定校验的服务端快照，不是 BLE 固件回帧。
  const cloudConfirmed = source === 'guardian' && options.cloudConfirmed === true;
  return {
    eventKey,
    deviceId,
    boot,
    id,
    alarmType,
    label: ALARM_LABELS[alarmType],
    active,
    source,
    origin,
    // 只有当前 BLE 链路（或显式的本地 device 调用）能证明固件回帧；
    // guardian/cloud 的 origin 字段只是服务端记录，不能直接消警。
    deviceConfirmed,
    cloudConfirmed,
    timestamp: Number.isFinite(timestamp) ? timestamp : Date.now()
  };
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

function createAlarmRuntime(options = {}) {
  const clock = options.clock || (() => Date.now());
  const storage = options.storage || defaultStorage();
  const storageKey = options.storageKey || DEFAULT_HISTORY_KEY;
  const maxHistory = Math.max(1, Number(options.maxHistory) || 80);
  const listeners = new Set();
  const records = new Map();
  let restored = null;
  try { restored = storage.read(storageKey); } catch (error) { restored = null; }
  if (Array.isArray(restored)) {
    restored.slice(0, maxHistory).forEach((item) => {
      if (item && item.eventKey && item.deviceId) records.set(item.eventKey, {
        ...item,
        acknowledgedBy: Array.isArray(item.acknowledgedBy) ? item.acknowledgedBy.slice() : [],
        sources: Array.isArray(item.sources) ? item.sources.slice() : [String(item.source || 'unknown')],
        // 仅影响本机当前展示；绝不能改变设备报警事件的 active/status。
        locallyHidden: item.locallyHidden === true
      });
    });
  }

  function sortRecords(items) {
    return items.sort((left, right) => (Number(right.timestamp || right.firstSeenAt) || 0) - (Number(left.timestamp || left.firstSeenAt) || 0));
  }

  function getAllActiveRecords() {
    return sortRecords(Array.from(records.values()).filter((item) => item.status === 'active'));
  }

  function getActiveRecords() {
    return getAllActiveRecords().filter((item) => item.locallyHidden !== true);
  }

  function getHistoryRecords() {
    return sortRecords(Array.from(records.values())).slice(0, maxHistory);
  }

  function snapshot() {
    const allActive = getAllActiveRecords();
    const active = allActive.filter((item) => item.locallyHidden !== true);
    const history = getHistoryRecords();
    return {
      active: clone(active),
      // allActive 仅供状态机收敛和安全音频判断使用，页面只显示 active。
      allActive: clone(allActive),
      hiddenActiveCount: allActive.length - active.length,
      history: clone(history),
      latest: history[0] ? clone(history[0]) : null
    };
  }

  function persist() {
    try { storage.write(storageKey, getHistoryRecords()); } catch (error) { /* 本地历史失败不影响实时报警。 */ }
  }

  function emit() {
    const current = snapshot();
    listeners.forEach((listener) => listener(current));
  }

  function stopAudioIfSafe() {
    if (!getAllActiveRecords().length && typeof options.onAllResolved === 'function') options.onAllResolved();
  }

  function mergeSource(record, source) {
    const nextSources = Array.from(new Set([...(record.sources || []), source]));
    const changed = nextSources.length !== (record.sources || []).length;
    record.sources = nextSources;
    return changed;
  }

  function ingest(input, context = {}) {
    let event;
    try {
      event = normalizeAlarmEvent(input, {
        ...context,
        cloudConfirmed: typeof options.authorizeCloudSnapshot === 'function'
          && options.authorizeCloudSnapshot(input, context) === true
      });
    } catch (error) {
      return { accepted: false, reason: 'invalid-event', error };
    }
    const receivedAt = nowValue(clock);
    const current = records.get(event.eventKey);

    // 普通云端回执不是固件消警；只有服务端确认快照可以更新监护端本地展示。
    if (!event.active && !event.deviceConfirmed && (!event.cloudConfirmed
      || (current && (current.deviceConfirmed || current.sources.includes('ble'))))) {
      if (current) {
        const sourceChanged = mergeSource(current, event.source);
        if (!sourceChanged && current.cloudActive === false) return { accepted: false, reason: 'cloud-resolution-not-authoritative', event: clone(current) };
        current.cloudActive = false;
        current.cloudReceiptAt = receivedAt;
        current.lastSeenAt = receivedAt;
        records.set(event.eventKey, current);
        persist();
        emit();
      }
      return { accepted: false, reason: 'cloud-resolution-not-authoritative', event: current ? clone(current) : null };
    }

    if (!current) {
      const record = {
        ...event,
        status: event.active ? 'active' : 'resolved',
        acknowledgedBy: [],
        firstSeenAt: receivedAt,
        lastSeenAt: receivedAt,
        resolvedAt: event.active ? null : receivedAt,
        sources: [event.source],
        locallyHidden: false
      };
      records.set(event.eventKey, record);
      persist();
      emit();
      if (event.active && typeof options.onAttention === 'function') options.onAttention(clone(record));
      if (!event.active) stopAudioIfSafe();
      return { accepted: true, action: event.active ? 'activated' : 'resolved', event: clone(record) };
    }

    if (current.alarmType !== event.alarmType) {
      return { accepted: false, reason: 'event-type-mismatch', event: clone(current) };
    }

    const sourceChanged = mergeSource(current, event.source);
    current.lastSeenAt = receivedAt;
    if (current.status === 'resolved') {
      // 同一个 device/BOOT/ID 不允许被旧的重复 ACTIVE 帧重新激活。
      if (sourceChanged) {
        persist();
        emit();
      }
      return { accepted: false, reason: 'already-resolved', event: clone(current) };
    }

    if (event.active) {
      const wasLocallyHidden = current.locallyHidden === true;
      current.origin = current.origin || event.origin;
      current.deviceConfirmed = current.deviceConfirmed || event.deviceConfirmed;
      current.cloudConfirmed = current.cloudConfirmed || event.cloudConfirmed;
      if (wasLocallyHidden) {
        // 设备再次证明事件仍活动时，必须撤销“仅隐藏本机”的展示选择。
        current.locallyHidden = false;
        current.lastAction = 'revealed-active';
        persist();
        emit();
        if (typeof options.onAttention === 'function') options.onAttention(clone(current));
        return { accepted: true, action: 'revealed-active', event: clone(current) };
      }
      if (sourceChanged) {
        persist();
        emit();
      }
      return { accepted: false, reason: 'duplicate-active', event: clone(current) };
    }

    current.active = false;
    current.status = 'resolved';
    current.locallyHidden = false;
    current.resolvedAt = receivedAt;
    current.resolutionSource = event.source;
    current.deviceConfirmed = current.deviceConfirmed || event.deviceConfirmed;
    current.cloudConfirmed = current.cloudConfirmed || event.cloudConfirmed;
    persist();
    emit();
    if (typeof options.onResolved === 'function') options.onResolved(clone(current));
    stopAudioIfSafe();
    return { accepted: true, action: 'resolved', event: clone(current) };
  }

  function find(eventKey) {
    const item = records.get(String(eventKey || ''));
    return item ? clone(item) : null;
  }

  function acknowledge(eventKey, actor = 'local') {
    const item = records.get(String(eventKey || ''));
    if (!item || item.status !== 'active') return { accepted: false, reason: 'not-active', event: item ? clone(item) : null };
    item.acknowledgedBy = Array.from(new Set([...(item.acknowledgedBy || []), String(actor)]));
    item.acknowledgedAt = nowValue(clock);
    item.lastAction = 'acknowledge';
    persist();
    emit();
    if (typeof options.onAcknowledged === 'function') options.onAcknowledged(clone(item), String(actor));
    return { accepted: true, action: 'acknowledged', event: clone(item) };
  }

  function hideLocally(eventKey) {
    const item = records.get(String(eventKey || ''));
    if (!item || item.status !== 'active') return { accepted: false, reason: 'not-active', event: item ? clone(item) : null };
    if (!(item.acknowledgedBy || []).includes('local')) {
      return { accepted: false, reason: 'acknowledge-first', event: clone(item) };
    }
    if (item.locallyHidden === true) return { accepted: false, reason: 'already-hidden', event: clone(item) };
    // 这不是消警：保留 active/status，供 ALARM_STATE 和 BLE ACTIVE=0 正确收敛。
    item.locallyHidden = true;
    item.locallyHiddenAt = nowValue(clock);
    item.lastAction = 'locally-hidden';
    persist();
    emit();
    return { accepted: true, action: 'locally-hidden', event: clone(item) };
  }

  function requestResolve(eventKey, actor = 'wearer') {
    const item = records.get(String(eventKey || ''));
    if (actor !== 'wearer') return { accepted: false, reason: 'remote-resolve-forbidden', event: item ? clone(item) : null };
    if (!item || item.status !== 'active') return { accepted: false, reason: 'not-active', event: item ? clone(item) : null };
    item.resolveRequestedAt = nowValue(clock);
    item.resolveRequestedBy = 'wearer';
    item.lastAction = 'resolve-requested';
    persist();
    emit();
    return { accepted: true, action: 'resolve-requested', event: clone(item) };
  }

  return {
    acknowledge,
    getState: snapshot,
    find,
    hideLocally,
    ingest,
    requestResolve,
    subscribe(listener) {
      listeners.add(listener);
      listener(snapshot());
      return () => listeners.delete(listener);
    },
    reset() {
      records.clear();
      persist();
      emit();
    }
  };
}

module.exports = { ALARM_LABELS, DEFAULT_HISTORY_KEY, createAlarmRuntime, normalizeAlarmEvent };
