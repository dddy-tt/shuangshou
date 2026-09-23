const STORAGE_KEY = 'shuangshou.remote-devices.v1';
const SELECTED_DEVICE_KEY = 'shuangshou.remote-selected-device.v1';
const BINDINGS_STORAGE_KEY = 'shuangshou.remote-gesture-bindings.v1';
const LEGACY_DEVICE_ID = 'light';
const LEGACY_TOPICS = Object.freeze({
  command: 'shuangshou/control/light',
  state: 'shuangshou/status/light',
  availability: ''
});
const DEVICE_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

function clone(value) {
  return value === undefined ? value : JSON.parse(JSON.stringify(value));
}

function getWxStorage(key) {
  if (typeof wx !== 'undefined' && wx.getStorageSync && wx.setStorageSync) {
    return {
      read: () => wx.getStorageSync(key),
      write: (value) => wx.setStorageSync(key, value),
      remove: () => wx.removeStorageSync && wx.removeStorageSync(key)
    };
  }

  let value;
  return {
    read: () => value,
    write: (next) => { value = next; },
    remove: () => { value = undefined; }
  };
}

function normalizeAction(action) {
  const value = String(action || '').trim().toUpperCase();
  if (value === 'LIGHT_ON') return 'ON';
  if (value === 'LIGHT_OFF') return 'OFF';
  return ['ON', 'OFF'].includes(value) ? value : '';
}

function normalizeId(value) {
  const id = String(value || '').trim();
  if (!id || !DEVICE_ID_PATTERN.test(id)) {
    throw new Error('设备唯一 ID 只能包含字母、数字、点、下划线或短横线，长度为 1~64。');
  }
  // 新 ESP 模板使用大写 chipId。保留 legacy 的小写 light 主题，避免
  // 已部署的旧单路固件被迁移到 LIGHT 主题。
  return id.toLowerCase() === LEGACY_DEVICE_ID ? LEGACY_DEVICE_ID : id.toUpperCase();
}

function isSameId(left, right) {
  return String(left || '').toLowerCase() === String(right || '').toLowerCase();
}

function topicsForId(id) {
  const normalizedId = normalizeId(id);
  if (isSameId(normalizedId, LEGACY_DEVICE_ID)) return { ...LEGACY_TOPICS };
  return {
    command: `shuangshou/control/${normalizedId}`,
    state: `shuangshou/status/${normalizedId}`,
    availability: `shuangshou/availability/${normalizedId}`
  };
}

function normalizeDevice(input = {}, options = {}) {
  const id = normalizeId(input.id || input.deviceId || input.chipId);
  const topics = topicsForId(id);
  const now = new Date().toISOString();
  const createdAt = input.createdAt || now;
  const commandFormat = input.commandFormat === 'json' ? 'json' : 'text';
  const name = String(input.name || input.title || id).trim() || id;

  return {
    id,
    deviceId: id,
    name,
    type: 'relay',
    legacy: isSameId(id, LEGACY_DEVICE_ID),
    commandTopic: topics.command,
    stateTopic: topics.state,
    controlTopic: topics.command,
    statusTopic: topics.state,
    availabilityTopic: topics.availability,
    topics,
    commandFormat,
    enabled: input.enabled !== false,
    createdAt,
    updatedAt: options.touch ? now : (input.updatedAt || createdAt)
  };
}

function createDeviceStore(storage, selectionStorage, bindingStorage) {
  const deviceStorage = storage || getWxStorage(STORAGE_KEY);
  const selectedStorage = selectionStorage || getWxStorage(SELECTED_DEVICE_KEY);
  const gestureBindingStorage = bindingStorage || getWxStorage(BINDINGS_STORAGE_KEY);
  let fallbackSelectedId = null;

  function readRaw() {
    const value = typeof deviceStorage.read === 'function' ? deviceStorage.read() : undefined;
    if (Array.isArray(value)) return { stored: value, devices: value };
    if (value && Array.isArray(value.devices)) return { stored: value, devices: value.devices };
    return { stored: value, devices: [] };
  }

  function readDevices() {
    const raw = readRaw();
    if (!raw.devices.length && (raw.stored === undefined || raw.stored === null || raw.stored === '')) {
      return [normalizeDevice({ id: LEGACY_DEVICE_ID, name: '灯', commandFormat: 'text' })];
    }

    const seen = new Set();
    return raw.devices.map((item) => {
      try { return normalizeDevice(item); } catch (error) { return null; }
    }).filter((item) => {
      if (!item) return false;
      const key = item.id.toLowerCase();
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
  }

  function writeDevices(devices) {
    if (typeof deviceStorage.write === 'function') deviceStorage.write(clone(devices));
    return devices.map(clone);
  }

  function readSelectedId() {
    if (typeof selectedStorage.read === 'function') {
      const value = selectedStorage.read();
      return value ? String(value) : null;
    }
    return fallbackSelectedId;
  }

  function writeSelectedId(id) {
    fallbackSelectedId = id || null;
    if (typeof selectedStorage.write === 'function') selectedStorage.write(fallbackSelectedId);
  }

  function findDevice(devices, id) {
    return devices.find((item) => isSameId(item.id, id)) || null;
  }

  function ensureSelection(devices) {
    const selected = findDevice(devices, readSelectedId());
    if (selected) return selected.id;
    const next = devices[0] ? devices[0].id : null;
    writeSelectedId(next);
    return next;
  }

  function readBindingMap() {
    const value = typeof gestureBindingStorage.read === 'function' ? gestureBindingStorage.read() : undefined;
    if (Array.isArray(value)) {
      return value.reduce((map, item) => {
        if (item && item.gestureId) map[item.gestureId] = item;
        return map;
      }, {});
    }
    return value && typeof value === 'object' ? value : {};
  }

  function writeBindingMap(map) {
    if (typeof gestureBindingStorage.write === 'function') gestureBindingStorage.write(clone(map));
    return map;
  }

  function cleanBindings(devices) {
    const validIds = new Set(devices.map((item) => item.id.toLowerCase()));
    const map = readBindingMap();
    const cleaned = {};
    Object.keys(map).forEach((gestureId) => {
      const binding = map[gestureId];
      if (!binding) return;
      const key = String(binding.gestureId || gestureId).trim();
      if (!key) return;
      const action = normalizeAction(binding.action);
      const followSelected = binding.mode === 'follow-selected' || binding.followSelected === true;
      const deviceId = binding.deviceId || binding.targetDeviceId || '';
      const device = deviceId && validIds.has(String(deviceId).toLowerCase())
        ? findDevice(devices, deviceId)
        : null;
      const updatedAt = binding.updatedAt || new Date().toISOString();

      if (followSelected) {
        cleaned[key] = {
          gestureId: key,
          deviceId: null,
          mode: 'follow-selected',
          disabled: false,
          action,
          updatedAt
        };
        return;
      }

      if (binding.disabled === true || binding.mode === 'disabled' || !device) {
        // 删除设备后保留不可用绑定，禁止下次刷新时静默落到当前选择设备。
        cleaned[key] = {
          gestureId: key,
          deviceId: deviceId ? String(deviceId).trim() : null,
          mode: 'disabled',
          disabled: true,
          reason: binding.reason || 'device-unavailable',
          action,
          removedAt: binding.removedAt || undefined,
          updatedAt
        };
        if (cleaned[key].removedAt === undefined) delete cleaned[key].removedAt;
        return;
      }

      cleaned[key] = {
        gestureId: key,
        deviceId: device.id,
        mode: 'device',
        disabled: false,
        action,
        updatedAt
      };
    });
    writeBindingMap(cleaned);
    return cleaned;
  }

  function list() {
    const devices = readDevices();
    ensureSelection(devices);
    return clone(devices);
  }

  function get(id) {
    return clone(findDevice(list(), id));
  }

  function add(input = {}) {
    const devices = readDevices();
    let id = input.id || input.deviceId || input.chipId;
    if (!id) {
      const base = `device_${Date.now().toString(36)}`;
      id = base;
      let suffix = 1;
      while (findDevice(devices, id)) id = `${base}_${suffix++}`;
    }
    id = normalizeId(id);
    if (findDevice(devices, id)) throw new Error(`设备 ID 已存在：${id}`);
    const device = normalizeDevice({ ...input, id });
    writeDevices([...devices, device]);
    if (!readSelectedId()) writeSelectedId(device.id);
    return clone(device);
  }

  function update(id, patch = {}) {
    const devices = readDevices();
    const current = findDevice(devices, id);
    if (!current) throw new Error(`设备不存在：${id}`);
    const requestedId = patch.id || patch.deviceId || current.id;
    const nextId = normalizeId(requestedId);
    const duplicate = devices.find((item) => !isSameId(item.id, current.id) && isSameId(item.id, nextId));
    if (duplicate) throw new Error(`设备 ID 已存在：${nextId}`);
    const updated = normalizeDevice({ ...current, ...patch, id: nextId }, { touch: true });
    const nextDevices = devices.map((item) => isSameId(item.id, current.id) ? updated : item);
    writeDevices(nextDevices);

    if (isSameId(readSelectedId(), current.id)) writeSelectedId(updated.id);
    if (!isSameId(current.id, updated.id)) {
      const bindings = readBindingMap();
      Object.keys(bindings).forEach((gestureId) => {
        if (bindings[gestureId]
          && bindings[gestureId].mode !== 'follow-selected'
          && isSameId(bindings[gestureId].deviceId || bindings[gestureId].targetDeviceId, current.id)) {
          bindings[gestureId] = { ...bindings[gestureId], deviceId: updated.id, updatedAt: new Date().toISOString() };
        }
      });
      writeBindingMap(bindings);
    }
    return clone(updated);
  }

  function remove(id) {
    const devices = readDevices();
    const current = findDevice(devices, id);
    if (!current) return false;
    const nextDevices = devices.filter((item) => !isSameId(item.id, current.id));
    writeDevices(nextDevices);
    if (isSameId(readSelectedId(), current.id)) ensureSelection(nextDevices);
    const bindings = readBindingMap();
    Object.keys(bindings).forEach((gestureId) => {
      if (bindings[gestureId]
        && isSameId(bindings[gestureId].deviceId || bindings[gestureId].targetDeviceId, current.id)) {
        const now = new Date().toISOString();
        bindings[gestureId] = {
          ...bindings[gestureId],
          gestureId: String(bindings[gestureId].gestureId || gestureId),
          deviceId: current.id,
          mode: 'disabled',
          disabled: true,
          reason: 'device-removed',
          removedAt: now,
          updatedAt: now
        };
      }
    });
    writeBindingMap(bindings);
    return true;
  }

  function select(id) {
    const device = findDevice(list(), id);
    if (!device) throw new Error(`设备不存在：${id}`);
    writeSelectedId(device.id);
    return clone(device);
  }

  function selectedId() {
    const devices = list();
    return ensureSelection(devices);
  }

  function selected() {
    return get(selectedId());
  }

  function listBindings() {
    const devices = list();
    return clone(Object.values(cleanBindings(devices)));
  }

  function getGestureBinding(gestureId) {
    if (!gestureId) return null;
    const binding = cleanBindings(list())[gestureId];
    return clone(binding || null);
  }

  function bindGesture(gestureId, deviceId, action, options = {}) {
    const key = String(gestureId || '').trim();
    if (!key) throw new Error('手势 ID 不能为空。');
    const normalizedAction = normalizeAction(action);
    if (action && !normalizedAction) throw new Error('手势控制动作只能是 ON 或 OFF。');
    const bindings = cleanBindings(list());
    const followSelected = options.mode === 'follow-selected' || options.followSelected === true;
    if (followSelected) {
      bindings[key] = {
        gestureId: key,
        deviceId: null,
        mode: 'follow-selected',
        disabled: false,
        action: normalizedAction,
        updatedAt: new Date().toISOString()
      };
    } else {
      const device = get(deviceId);
      if (!device) throw new Error(`设备不存在：${deviceId}`);
      bindings[key] = {
        gestureId: key,
        deviceId: device.id,
        mode: 'device',
        disabled: false,
        action: normalizedAction,
        updatedAt: new Date().toISOString()
      };
    }
    writeBindingMap(bindings);
    return clone(bindings[key]);
  }

  function followSelectedGesture(gestureId, action) {
    return bindGesture(gestureId, null, action, { mode: 'follow-selected' });
  }

  function unbindGesture(gestureId) {
    const bindings = cleanBindings(list());
    const key = String(gestureId || '').trim();
    if (!key || !bindings[key]) return false;
    delete bindings[key];
    writeBindingMap(bindings);
    return true;
  }

  return {
    list,
    get,
    add,
    update,
    remove,
    select,
    selectedId,
    selected,
    listBindings,
    getGestureBinding,
    bindGesture,
    followSelectedGesture,
    unbindGesture
  };
}

module.exports = {
  STORAGE_KEY,
  SELECTED_DEVICE_KEY,
  BINDINGS_STORAGE_KEY,
  LEGACY_DEVICE_ID,
  LEGACY_TOPICS,
  DEVICE_ID_PATTERN,
  normalizeAction,
  normalizeId,
  topicsForId,
  normalizeDevice,
  createDeviceStore
};
