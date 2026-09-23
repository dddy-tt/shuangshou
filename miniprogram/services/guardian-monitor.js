function errorMessage(error) {
  return error && (error.message || error.errMsg || error.msg) || String(error || '监护轮询失败');
}

function normalizeStatus(value, offlineAfterMs, clock) {
  const status = value || {};
  const lastSeenAt = Number(status.lastSeenAt) || 0;
  const now = Number(clock());
  const onlineByHeartbeat = lastSeenAt > 0 && Number.isFinite(now) && now - lastSeenAt <= offlineAfterMs;
  return {
    online: status.online === true && onlineByHeartbeat,
    lastSeenAt,
    wearableId: String(status.wearableId || ''),
    statusText: status.online === true && onlineByHeartbeat ? '佩戴者在线' : lastSeenAt ? '暂未收到最近心跳' : '尚未收到在线状态'
  };
}

function createGuardianMonitor(options = {}) {
  const api = options.api;
  const clock = options.clock || (() => Date.now());
  const intervalMs = Math.max(3000, Number(options.pollIntervalMs) || 15000);
  const offlineAfterMs = Math.max(intervalMs, Number(options.offlineAfterMs) || 60000);
  const onEvent = options.onEvent || function noop() {};
  const onStatus = options.onStatus || function noop() {};
  const onError = options.onError || function noop() {};
  let running = false;
  let timer = null;
  let generation = 0;
  let activePolls = 0;
  let currentPoll = null;
  let lastPoll = null;
  let status = { online: false, lastSeenAt: 0, wearableId: '', statusText: '尚未开始监护轮询' };

  function clearTimer() {
    if (timer) clearTimeout(timer);
    timer = null;
  }

  async function poll(currentGeneration = generation) {
    if (!running || !api || currentGeneration !== generation) return;
    if (currentPoll && currentPoll.generation === currentGeneration) return currentPoll.promise;
    activePolls += 1;
    let operation;
    operation = (async () => {
    try {
      const queryContext = typeof options.getQueryContext === 'function' ? await options.getQueryContext() : null;
      if (!running || currentGeneration !== generation) return;
      const [eventsResult, statusResult] = await Promise.all([
        api.listEvents({ limit: options.eventPageSize }).then((value) => ({ value })).catch((error) => ({ error })),
        (api.getStatus ? api.getStatus() : api.call('getStatus')).then((value) => ({ value })).catch((error) => ({ error }))
      ]);
      if (!running || currentGeneration !== generation) return;
      if (eventsResult.error) onError(eventsResult.error);
      else {
        const events = eventsResult.value.events || eventsResult.value.items || [];
        events.slice().reverse().forEach((event) => onEvent({
          type: 'alarm',
          ...event,
          source: 'guardian',
          deviceId: event.deviceId || event.wearableId,
          // 保留服务端认证后的快照标志；app-state 仍需校验当前 binding 设备，
          // 并把它作为 cloudConfirmed 传给运行时，不能当作监护端收到的 BLE 回帧。
          deviceConfirmed: event.deviceConfirmed === true
        }, queryContext));
      }
      if (statusResult.error) onError(statusResult.error);
      else {
        status = normalizeStatus(statusResult.value.status || statusResult.value, offlineAfterMs, clock);
        onStatus(status);
      }
    } catch (error) {
      if (running && currentGeneration === generation) onError(error);
    } finally {
      activePolls -= 1;
      if (currentPoll && currentPoll.promise === operation) currentPoll = null;
      if (running && currentGeneration === generation) timer = setTimeout(poll, intervalMs);
    }
    })();
    currentPoll = { generation: currentGeneration, promise: operation };
    return operation;
  }

  function start() {
    if (running) return lastPoll || Promise.resolve();
    running = true;
    generation += 1;
    clearTimer();
    lastPoll = poll();
    return lastPoll;
  }

  function stop() {
    running = false;
    generation += 1;
    clearTimer();
    lastPoll = null;
  }

  return {
    getState: () => ({ running, inFlight: activePolls > 0, status: { ...status } }),
    poll: () => {
      if (!running) return Promise.resolve();
      clearTimer();
      lastPoll = poll();
      return lastPoll;
    },
    start,
    stop,
    close: stop,
    errorMessage
  };
}

module.exports = { createGuardianMonitor, errorMessage, normalizeStatus };
