const FLEX_COUNT = 10;
const DEFAULT_INTERVAL_MS = 250;
const STATUS_INTERVAL_MS = 1000;

const PRESETS = Object.freeze({
  normal_pose: {
    label: '正常静止',
    flex: [8, 10, 9, 8, 7, 8, 10, 9, 8, 7],
    imu: { roll: 0, pitch: 0, yaw: 0 },
    acc: { x: 0, y: 0, z: 1 }
  },
  all_straight: {
    label: '全部伸直',
    flex: Array(FLEX_COUNT).fill(0),
    imu: { roll: 0, pitch: 0, yaw: 0 },
    acc: { x: 0, y: 0, z: 1 }
  },
  all_half: {
    label: '全部半弯',
    flex: Array(FLEX_COUNT).fill(50),
    imu: { roll: 0, pitch: 0, yaw: 0 },
    acc: { x: 0, y: 0, z: 1 }
  },
  all_bent: {
    label: '全部全弯',
    flex: Array(FLEX_COUNT).fill(100),
    imu: { roll: 0, pitch: 0, yaw: 0 },
    acc: { x: 0, y: 0, z: 1 }
  },
  mixed_fingers: {
    label: '混合十指',
    flex: [5, 25, 45, 75, 95, 90, 70, 50, 30, 10],
    imu: { roll: 12, pitch: -6, yaw: 18 },
    acc: { x: 0.08, y: -0.04, z: 0.99 }
  }
});

function number(value, name, min, max) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < min || parsed > max) {
    throw new Error(`${name} 必须在 ${min} 到 ${max} 之间`);
  }
  return parsed;
}

function cloneSnapshot(snapshot) {
  return {
    flex: snapshot.flex.slice(),
    imu: { ...snapshot.imu },
    acc: { ...snapshot.acc }
  };
}

function normalizeSnapshot(input = {}) {
  const source = input || {};
  const flex = Array.isArray(source.flex) ? source.flex : PRESETS.normal_pose.flex;
  if (flex.length !== FLEX_COUNT) throw new Error('虚拟 FLEX 必须恰好包含 10 个数值');
  return {
    flex: flex.map((value, index) => number(value, `FLEX[${index}]`, 0, 100)),
    imu: {
      roll: number(source.imu && source.imu.roll !== undefined ? source.imu.roll : 0, 'Roll', -180, 180),
      pitch: number(source.imu && source.imu.pitch !== undefined ? source.imu.pitch : 0, 'Pitch', -180, 180),
      yaw: number(source.imu && source.imu.yaw !== undefined ? source.imu.yaw : 0, 'Yaw', -360, 360)
    },
    acc: {
      x: number(source.acc && source.acc.x !== undefined ? source.acc.x : 0, 'ACC X', -16, 16),
      y: number(source.acc && source.acc.y !== undefined ? source.acc.y : 0, 'ACC Y', -16, 16),
      z: number(source.acc && source.acc.z !== undefined ? source.acc.z : 1, 'ACC Z', -16, 16)
    }
  };
}

function formatNumber(value, digits = 2) {
  return Number(value).toFixed(digits);
}

function buildTelemetryFrames(input, options = {}) {
  const snapshot = normalizeSnapshot(input);
  const lines = [];
  if (options.includeBoot) lines.push('BOOT:VIRTUAL_GLOVE');
  if (options.includeStatus) {
    lines.push('BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1');
    lines.push('JY|ONLINE=1|ERR=0|LAST=0|AGE=0');
  }
  const left = snapshot.flex.slice(0, 5).map((value, index) => `L${index + 1}=${formatNumber(value, 2)}`);
  const right = snapshot.flex.slice(5).map((value, index) => `R${index + 1}=${formatNumber(value, 2)}`);
  lines.push(`FLEX|${left.concat(right).join('|')}`);
  lines.push(`IMU|R=${formatNumber(snapshot.imu.roll)}|P=${formatNumber(snapshot.imu.pitch)}|Y=${formatNumber(snapshot.imu.yaw)}`);
  lines.push(`ACC|X=${formatNumber(snapshot.acc.x, 3)}|Y=${formatNumber(snapshot.acc.y, 3)}|Z=${formatNumber(snapshot.acc.z, 3)}|VALID=1`);
  return lines;
}

function encodeUtf8(text) {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(text).buffer;
  const encoded = unescape(encodeURIComponent(text));
  const bytes = new Uint8Array(encoded.length);
  for (let index = 0; index < encoded.length; index += 1) bytes[index] = encoded.charCodeAt(index);
  return bytes.buffer;
}

function emitLines(lines, onData, chunkBytes) {
  const payload = encodeUtf8(`${lines.join('\r\n')}\r\n`);
  if (!Number.isInteger(chunkBytes) || chunkBytes <= 0) {
    onData(payload);
    return;
  }
  const bytes = new Uint8Array(payload);
  for (let start = 0; start < bytes.length; start += chunkBytes) {
    onData(bytes.slice(start, start + chunkBytes).buffer);
  }
}

function createSimulator(options = {}) {
  const onData = options.onData || function noop() {};
  const onStateChange = options.onStateChange || function noop() {};
  const now = options.now || (() => Date.now());
  const schedule = options.setInterval || setInterval;
  const cancel = options.clearInterval || clearInterval;
  const intervalMs = Number.isFinite(options.intervalMs) ? options.intervalMs : DEFAULT_INTERVAL_MS;
  let snapshot = normalizeSnapshot(options.initialSnapshot || PRESETS.normal_pose);
  let running = false;
  let active = false;
  let timer = null;
  let hasBooted = false;
  let lastStatusAt = 0;
  let lastInjectedAt = 0;

  function state() {
    return {
      active,
      running,
      intervalMs,
      lastInjectedAt,
      snapshot: cloneSnapshot(snapshot)
    };
  }

  function publishState() {
    onStateChange(state());
  }

  function inject(options = {}) {
    const timestamp = now();
    const includeBoot = options.includeBoot === true || !hasBooted;
    const includeStatus = options.includeStatus === true
      || !lastStatusAt
      || timestamp - lastStatusAt >= STATUS_INTERVAL_MS;
    emitLines(buildTelemetryFrames(snapshot, { includeBoot, includeStatus }), onData, options.chunkBytes);
    hasBooted = true;
    if (includeStatus) lastStatusAt = timestamp;
    lastInjectedAt = timestamp;
    active = true;
    publishState();
    return state();
  }

  function tick() {
    inject();
  }

  return {
    getState: state,
    getSnapshot: () => cloneSnapshot(snapshot),
    setSnapshot(next, options = {}) {
      snapshot = normalizeSnapshot({
        flex: next.flex || snapshot.flex,
        imu: { ...snapshot.imu, ...(next.imu || {}) },
        acc: { ...snapshot.acc, ...(next.acc || {}) }
      });
      if (options.inject !== false) inject(options);
      else publishState();
      return state();
    },
    applyPreset(name, options = {}) {
      if (!Object.prototype.hasOwnProperty.call(PRESETS, name)) throw new Error(`未知虚拟手套预设：${name}`);
      snapshot = normalizeSnapshot(PRESETS[name]);
      if (options.inject !== false) inject(options);
      else publishState();
      return state();
    },
    loadCase(testCase, options = {}) {
      if (!testCase || typeof testCase !== 'object') throw new Error('测试案例必须是对象');
      return this.setSnapshot(testCase, options);
    },
    inject,
    start() {
      if (running) return state();
      running = true;
      inject({ includeBoot: true, includeStatus: true });
      timer = schedule(tick, intervalMs);
      publishState();
      return state();
    },
    stop() {
      if (timer) cancel(timer);
      timer = null;
      running = false;
      active = false;
      publishState();
      return state();
    },
    dispose() {
      return this.stop();
    }
  };
}

module.exports = {
  DEFAULT_INTERVAL_MS,
  PRESETS,
  buildTelemetryFrames,
  createSimulator,
  normalizeSnapshot
};
