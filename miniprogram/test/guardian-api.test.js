const assert = require('assert');
const { GuardianApiError, createGuardianApi } = require('../services/guardian-api');

const notConfigured = createGuardianApi({ wx: {} });
assert.strictEqual(notConfigured.isConfigured(), false);
notConfigured.getBinding().then(() => { throw new Error('未配置不应调用云函数'); }).catch((error) => {
  assert(error instanceof GuardianApiError);
  assert.strictEqual(error.code, 'GUARDIAN_NOT_CONFIGURED');

  const api = createGuardianApi({
    config: { envId: 'test-env' },
    wx: { cloud: { callFunction: ({ success }) => success({ result: { ok: true, action: 'getBinding' } }) } }
  });
  assert.strictEqual(api.isConfigured(), true);
  return api.getBinding().then((result) => {
    assert.strictEqual(result.action, 'getBinding');
    console.log('guardian api tests passed');
  });
}).catch((error) => { console.error(error); process.exitCode = 1; });
