const exampleConfig = require('../config/guardian.example');

function loadLocalConfig() {
  try { return require('../config/guardian.local'); } catch (error) { return {}; }
}

function errorMessage(error) {
  return error && (error.errMsg || error.message || error.msg) || String(error || '未知云函数错误');
}

class GuardianApiError extends Error {
  constructor(code, message, detail) {
    super(message);
    this.name = 'GuardianApiError';
    this.code = code;
    this.detail = detail;
  }
}

function createGuardianApi(options = {}) {
  const config = Object.freeze({ ...exampleConfig, ...loadLocalConfig(), ...(options.config || {}) });
  const wxApi = options.wx || (typeof wx !== 'undefined' ? wx : null);

  function isConfigured() {
    return Boolean(String(config.envId || '').trim() && String(config.functionName || '').trim());
  }

  function notConfigured() {
    return Promise.reject(new GuardianApiError(
      'GUARDIAN_NOT_CONFIGURED',
      '监护服务未配置：请先填写微信云开发环境 ID，并完成云函数部署。'
    ));
  }

  function call(action, data = {}) {
    if (!isConfigured()) return notConfigured();
    if (!wxApi || !wxApi.cloud || typeof wxApi.cloud.callFunction !== 'function') {
      return Promise.reject(new GuardianApiError('CLOUD_UNAVAILABLE', '当前微信环境不支持云开发能力。'));
    }
    return new Promise((resolve, reject) => wxApi.cloud.callFunction({
      name: config.functionName,
      data: { action, ...data },
      success: resolve,
      fail: reject
    })).then((result) => {
      const payload = result && result.result;
      if (!payload || payload.ok !== true) {
        throw new GuardianApiError(
          payload && payload.code || 'GUARDIAN_CALL_FAILED',
          payload && payload.message || '监护服务请求失败。',
          payload
        );
      }
      return payload;
    }).catch((error) => {
      if (error instanceof GuardianApiError) throw error;
      throw new GuardianApiError('GUARDIAN_NETWORK_ERROR', errorMessage(error), error);
    });
  }

  return {
    config,
    isConfigured,
    call,
    createInvite({ wearableId, ttlMs } = {}) {
      return call('createInvite', { wearableId, ttlMs });
    },
    acceptInvite(code) {
      return call('acceptInvite', { code: String(code || '').trim().toUpperCase() });
    },
    getBinding() { return call('getBinding'); },
    listEvents({ limit } = {}) {
      return call('listEvents', { limit: limit || config.eventPageSize });
    },
    getStatus() {
      return call('getStatus');
    },
    publishEvent(event) {
      return call('publishEvent', { event });
    },
    touchStatus({ wearableId, online } = {}) {
      return call('touchStatus', { wearableId, online: online !== false });
    },
    acknowledgeEvent(eventKey) {
      return call('acknowledgeEvent', { eventKey: String(eventKey || '') });
    },
    // 明确不暴露远程 resolve 接口，云端只能记录确认，不能代替固件 ACTIVE=0。
    resolveEvent() {
      return Promise.reject(new GuardianApiError('REMOTE_RESOLVE_FORBIDDEN', '监护者不能远程解除固件报警。'));
    }
  };
}

module.exports = { GuardianApiError, createGuardianApi, errorMessage, loadLocalConfig };
