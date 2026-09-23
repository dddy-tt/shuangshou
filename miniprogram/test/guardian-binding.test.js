const assert = require('assert');
const { createGuardianBindingService, ROLES } = require('../services/guardian-binding');

const values = new Map();
const api = {
  isConfigured: () => true,
  getBinding: () => Promise.resolve({ role: ROLES.WEARER, binding: { wearableId: 'device-a' } }),
  createInvite: () => Promise.resolve({ invite: { code: 'ABCD1234', expiresAt: 99 } }),
  acceptInvite: () => Promise.resolve({ binding: { wearableId: 'device-a' } })
};
const binding = createGuardianBindingService({
  api,
  storage: { read: (key) => values.get(key), write: (key, value) => values.set(key, value) }
});

assert.strictEqual(binding.getState().role, ROLES.UNSELECTED);
binding.selectRole(ROLES.WEARER);
return binding.createInvite('device-a').then((state) => {
  assert.strictEqual(state.invite.code, 'ABCD1234');
  binding.selectRole(ROLES.GUARDIAN);
  return binding.acceptInvite('ABCD1234');
}).then((state) => {
  assert.strictEqual(state.role, ROLES.GUARDIAN);
  assert.strictEqual(state.binding.wearableId, 'device-a');
  return binding.refresh();
}).then((state) => {
  assert.strictEqual(state.role, ROLES.WEARER, '服务端返回的角色用于覆盖本地选择');
  console.log('guardian binding tests passed');
}).catch((error) => { console.error(error); process.exitCode = 1; });
