const { formatAlarmAck } = require('../utils/protocol');

function detailOf(error) {
  return error && (error.errMsg || error.message) || String(error || '未知 ACK 错误');
}

function createAlarmAckController(options = {}) {
  const timeoutMs = Math.max(1, Number(options.timeoutMs) || 1500);
  const maxAttempts = Math.max(1, Number(options.maxAttempts) || 3);
  const retryDelayMs = Math.max(0, Number(options.retryDelayMs) || 0);
  const write = options.write;
  const getCurrentDeviceId = options.getCurrentDeviceId || (() => '');
  const getConnectionToken = options.getConnectionToken || (() => undefined);
  const onAttempt = options.onAttempt || function noop() {};
  const onWaiting = options.onWaiting || function noop() {};
  const onResult = options.onResult || function noop() {};
  const pending = new Map();

  function safeCurrentDeviceId() {
    try { return String(getCurrentDeviceId() || '').trim(); } catch (error) { return ''; }
  }

  function safeConnectionToken() {
    try { return getConnectionToken(); } catch (error) { return undefined; }
  }

  function wait(milliseconds) {
    if (!milliseconds) return Promise.resolve();
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
  }

  function withTimeout(promise, milliseconds) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('BLE ACK 写入超时')), milliseconds);
      Promise.resolve(promise).then((value) => {
        clearTimeout(timer);
        resolve(value);
      }, (error) => {
        clearTimeout(timer);
        reject(error);
      });
    });
  }

  function isCurrent(request) {
    if (typeof options.isCurrent === 'function') {
      try { return options.isCurrent(request.deviceId, request.connectionToken) === true; } catch (error) { return false; }
    }
    if (safeCurrentDeviceId() !== request.deviceId) return false;
    if (request.connectionToken === undefined) return true;
    return safeConnectionToken() === request.connectionToken;
  }

  function finish(request, result) {
    if (request.settled) return;
    request.settled = true;
    if (request.timer) clearTimeout(request.timer);
    request.timer = null;
    if (request.waitResolve) {
      const resolveWait = request.waitResolve;
      request.waitResolve = null;
      resolveWait(false);
    }
    pending.delete(request.key);
    const next = { ...result, key: request.key, attempts: request.attempts };
    request.resolve(next);
    try { onResult(next, request.event); } catch (error) { /* 状态回调不能破坏 ACK 清理 */ }
  }

  function waitForResolution(request) {
    if (request.resolved) return Promise.resolve(true);
    return new Promise((resolve) => {
      request.waitResolve = resolve;
      request.timer = setTimeout(() => {
        request.timer = null;
        request.waitResolve = null;
        resolve(false);
      }, timeoutMs);
    });
  }

  async function run(request) {
    for (let attempt = 1; attempt <= maxAttempts; attempt += 1) {
      if (request.settled) return;
      if (!isCurrent(request)) {
        finish(request, { ok: false, reason: 'stale-device' });
        return;
      }
      request.attempts = attempt;
      const command = request.command;
      try { onAttempt({ attempt, maxAttempts, command, key: request.key }, request.event); } catch (error) { /* 状态回调不能阻断写入 */ }

      let writeError = null;
      try {
        if (typeof write !== 'function') throw new Error('BLE 写通道不可用');
        await withTimeout(write(command), timeoutMs);
      } catch (error) {
        writeError = error;
      }
      if (request.resolved) {
        finish(request, { ok: true, reason: 'resolved-by-device' });
        return;
      }
      if (writeError) {
        if (attempt >= maxAttempts) {
          finish(request, { ok: false, reason: 'write-failed', detail: detailOf(writeError) });
          return;
        }
        await wait(retryDelayMs);
        continue;
      }
      if (!isCurrent(request)) {
        finish(request, { ok: false, reason: 'stale-device' });
        return;
      }
      try { onWaiting({ attempt, maxAttempts, command, key: request.key }, request.event); } catch (error) { /* 状态回调不能阻断 ACK 等待 */ }
      if (await waitForResolution(request)) {
        finish(request, { ok: true, reason: 'resolved-by-device' });
        return;
      }
      if (!isCurrent(request)) {
        finish(request, { ok: false, reason: 'stale-device' });
        return;
      }
    }
    finish(request, { ok: false, reason: 'active-zero-timeout' });
  }

  function request(event = {}, overrides = {}) {
    const deviceId = String(overrides.deviceId || event.deviceId || '').trim();
    const boot = Number(event.boot);
    const id = Number(event.id);
    if (!deviceId) return Promise.resolve({ ok: false, reason: 'stale-device' });
    let command;
    try { command = formatAlarmAck(boot, id); } catch (error) {
      return Promise.resolve({ ok: false, reason: 'invalid-event', detail: detailOf(error) });
    }
    const key = `${deviceId}/${boot}/${id}`;
    if (pending.has(key)) return pending.get(key).promise;
    const connectionToken = Object.prototype.hasOwnProperty.call(overrides, 'connectionToken')
      ? overrides.connectionToken
      : safeConnectionToken();
    const requestState = {
      key,
      event: { ...event, deviceId, boot, id },
      deviceId,
      boot,
      id,
      command,
      connectionToken,
      attempts: 0,
      resolved: false,
      settled: false,
      timer: null,
      waitResolve: null,
      promise: null,
      resolve: null
    };
    requestState.promise = new Promise((resolve) => { requestState.resolve = resolve; });
    pending.set(key, requestState);
    run(requestState).catch((error) => finish(requestState, { ok: false, reason: 'ack-failed', detail: detailOf(error) }));
    return requestState.promise;
  }

  function notifyResolved(event = {}, overrides = {}) {
    if (event.active !== false) return false;
    const deviceId = String(overrides.deviceId || event.deviceId || '').trim();
    const boot = Number(event.boot);
    const id = Number(event.id);
    const key = `${deviceId}/${boot}/${id}`;
    const requestState = pending.get(key);
    if (!requestState || !isCurrent(requestState)) return false;
    if (event.alarmType !== undefined && Number(event.alarmType) !== requestState.event.alarmType) return false;
    requestState.resolved = true;
    if (requestState.waitResolve) {
      const resolveWait = requestState.waitResolve;
      requestState.waitResolve = null;
      if (requestState.timer) clearTimeout(requestState.timer);
      requestState.timer = null;
      resolveWait(true);
    }
    return true;
  }

  function cancel(reason = 'cancelled') {
    Array.from(pending.values()).forEach((requestState) => finish(requestState, { ok: false, reason }));
  }

  return {
    cancel,
    getState: () => ({ pending: pending.size, keys: Array.from(pending.keys()) }),
    notifyResolved,
    request,
    stop: cancel
  };
}

module.exports = { createAlarmAckController };
