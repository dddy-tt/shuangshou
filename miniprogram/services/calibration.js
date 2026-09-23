const STORAGE_KEY = 'shuangshou.calibration.v1';

function defaultValue() {
  return { flexZero: null, flexFull: null, imuZero: null, enabledFingers: Array(10).fill(true) };
}

function clone(value) { return JSON.parse(JSON.stringify(value)); }
function normalizeEnabledFingers(value) {
  return Array.from({ length: 10 }, (_, index) => !Array.isArray(value) || value[index] !== false);
}
function normalizeCalibration(value) {
  const next = { ...defaultValue(), ...(value || {}) };
  next.enabledFingers = normalizeEnabledFingers(next.enabledFingers);
  return next;
}
function validFlex(values) { return Array.isArray(values) && values.length === 10 && values.every((value) => typeof value === 'number' && Number.isFinite(value)); }

function clamp(value) { return Math.max(0, Math.min(100, value)); }

function mapFlex(raw, zero, full) {
  if (!validFlex(raw) || !validFlex(zero) || !validFlex(full)) return raw.slice();
  return raw.map((value, index) => {
    const span = full[index] - zero[index];
    if (Math.abs(span) < 0.5) return clamp(Number(value));
    return clamp((Number(value) - zero[index]) / span * 100);
  });
}

function offsetImu(pose, zero) {
  if (!zero || !Number.isFinite(Number(pose.roll))) return { ...pose };
  return { roll: pose.roll - zero.roll, pitch: pose.pitch - zero.pitch, yaw: pose.yaw - zero.yaw };
}

function createCalibrationStore() {
  function read() {
    const saved = typeof wx !== 'undefined' && wx.getStorageSync ? wx.getStorageSync(STORAGE_KEY) : null;
    return normalizeCalibration(saved);
  }
  function write(value) {
    const next = normalizeCalibration(value);
    if (typeof wx !== 'undefined' && wx.setStorageSync) wx.setStorageSync(STORAGE_KEY, next);
    return clone(next);
  }
  return { read, write, mapFlex, offsetImu };
}

module.exports = { STORAGE_KEY, createCalibrationStore, mapFlex, offsetImu, validFlex, normalizeEnabledFingers };
