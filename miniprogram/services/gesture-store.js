const STORAGE_KEY = 'shuangshou.gesture-library.v1';
const EXPORT_VERSION = 1;

function getDefaultStorage() {
  if (typeof wx !== 'undefined' && wx.getStorageSync && wx.setStorageSync) {
    return {
      read: () => wx.getStorageSync(STORAGE_KEY),
      write: (value) => wx.setStorageSync(STORAGE_KEY, value)
    };
  }
  let value = [];
  return { read: () => value, write: (next) => { value = next; } };
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function normalizeGesture(input = {}, options = {}) {
  const now = new Date().toISOString();
  const fingers = Array.isArray(input.fingers) ? input.fingers.slice(0, 10) : [];
  const action = String(input.action || input.text || '').trim();
  const createdAt = input.createdAt || now;
  return {
    id: input.id || `gesture_${Date.now()}_${Math.random().toString(16).slice(2)}`,
    name: String(input.name || action || '未命名手势').trim(),
    text: action,
    action,
    category: ['translation', 'training', 'control'].includes(input.category) ? input.category : 'translation',
    fingers,
    states: Array.isArray(input.states) ? input.states.slice(0, 10) : [],
    pose: input.pose || null,
    poseTolerance: Number.isFinite(Number(input.poseTolerance)) ? Number(input.poseTolerance) : 8,
    matchPose: Boolean(input.matchPose),
    enabled: input.enabled !== false,
    createdAt,
    updatedAt: options.touch ? now : (input.updatedAt || createdAt)
  };
}

function createGestureStore(storage = getDefaultStorage()) {
  function read() {
    const value = storage.read();
    return Array.isArray(value) ? value.map((item) => normalizeGesture(item)) : [];
  }

  function write(items) {
    storage.write(items.map(normalizeGesture));
    return read();
  }

  function importJson(serialized, mode = 'merge') {
    const parsed = typeof serialized === 'string' ? JSON.parse(serialized) : serialized;
    const items = Array.isArray(parsed) ? parsed : parsed && parsed.items;
    if (!Array.isArray(items)) throw new Error('导入内容必须是手势数组或包含 items 的导出文件。');
    const normalized = items.map(normalizeGesture).filter((item) => item.text && item.fingers.length === 10);
    if (!normalized.length) throw new Error('导入内容中没有可用的十指手势。');
    if (mode === 'replace') return write(normalized);

    const existing = read();
    const existingIds = new Set(existing.map((item) => item.id));
    const additions = normalized.filter((item) => !existingIds.has(item.id));
    return write([...existing, ...additions]);
  }

  return {
    list() {
      return clone(read());
    },
    add(gesture) {
      return write([...read(), normalizeGesture(gesture)]);
    },
    update(id, patch) {
      return write(read().map((item) => item.id === id ? normalizeGesture({ ...item, ...patch, id }, { touch: true }) : item));
    },
    remove(id) {
      return write(read().filter((item) => item.id !== id));
    },
    exportJson() {
      return JSON.stringify({ version: EXPORT_VERSION, exportedAt: new Date().toISOString(), items: read() }, null, 2);
    },
    importJson,
    clear() {
      return write([]);
    }
  };
}

module.exports = { STORAGE_KEY, createGestureStore, normalizeGesture };
