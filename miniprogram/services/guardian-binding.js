const ROLE_KEY = 'shuangshou.guardian.role.v1';
const BINDING_KEY = 'shuangshou.guardian.binding.v1';
const ROLES = Object.freeze({ UNSELECTED: 'unselected', WEARER: 'wearer', GUARDIAN: 'guardian' });

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

function createGuardianBindingService(options = {}) {
  const api = options.api;
  const storage = options.storage || defaultStorage();
  let role = storage.read(ROLE_KEY) || ROLES.UNSELECTED;
  let binding = storage.read(BINDING_KEY) || null;
  let invite = null;
  if (!Object.values(ROLES).includes(role)) role = ROLES.UNSELECTED;

  function save() {
    storage.write(ROLE_KEY, role);
    storage.write(BINDING_KEY, binding);
  }

  function getState() {
    return { role, binding, invite, configured: Boolean(api && api.isConfigured && api.isConfigured()) };
  }

  function selectRole(nextRole) {
    if (![ROLES.WEARER, ROLES.GUARDIAN].includes(nextRole)) throw new Error('监护角色无效。');
    role = nextRole;
    save();
    return getState();
  }

  function requireApi() {
    if (!api) return Promise.reject(new Error('监护服务不可用。'));
    return null;
  }

  function refresh() {
    const unavailable = requireApi();
    if (unavailable) return unavailable;
    return api.getBinding().then((result) => {
      binding = result.binding || null;
      if (result.role === ROLES.WEARER || result.role === ROLES.GUARDIAN) role = result.role;
      save();
      return getState();
    });
  }

  function createInvite(wearableId, ttlMs) {
    if (role !== ROLES.WEARER) return Promise.reject(new Error('只有佩戴者可以生成绑定邀请码。'));
    const unavailable = requireApi();
    if (unavailable) return unavailable;
    return api.createInvite({ wearableId, ttlMs }).then((result) => {
      invite = result.invite || null;
      return getState();
    });
  }

  function acceptInvite(code) {
    if (role !== ROLES.GUARDIAN) return Promise.reject(new Error('请先选择监护者角色。'));
    const unavailable = requireApi();
    if (unavailable) return unavailable;
    return api.acceptInvite(code).then((result) => {
      binding = result.binding || null;
      invite = null;
      save();
      return getState();
    });
  }

  return {
    acceptInvite,
    createInvite,
    getState,
    refresh,
    selectRole,
    clearLocal() {
      role = ROLES.UNSELECTED;
      binding = null;
      invite = null;
      save();
      return getState();
    },
    roles: ROLES
  };
}

module.exports = { BINDING_KEY, ROLE_KEY, ROLES, createGuardianBindingService };
