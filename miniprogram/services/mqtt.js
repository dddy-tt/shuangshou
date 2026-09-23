const MQTT_CONFIG = Object.freeze({
  url: 'wxs://broker.emqx.io:8084/mqtt',
  clientId: '',
  commandFormat: 'text',
  username: '',
  password: '',
  pendingTimeoutMs: 8000,
  heartbeatTimeoutMs: 45000,
  topics: {
    command: 'shuangshou/control/light',
    state: 'shuangshou/status/light',
    availability: ''
  }
});
const DEVICE_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

function clone(value) {
  return value === undefined ? value : JSON.parse(JSON.stringify(value));
}

function normalizeAction(value) {
  const action = String(value || '').trim().toUpperCase();
  if (action === 'LIGHT_ON') return 'ON';
  if (action === 'LIGHT_OFF') return 'OFF';
  return ['ON', 'OFF'].includes(action) ? action : '';
}

function normalizeDeviceId(device) {
  const raw = device && typeof device === 'object'
    ? String(device.id || device.deviceId || device.chipId || '').trim()
    : String(device || '').trim();
  if (!raw) return '';
  // ESP 模板统一生成大写 chipId；legacy light 必须保留原主题大小写。
  return raw.toLowerCase() === 'light' ? 'light' : raw.toUpperCase();
}

function sameDeviceId(left, right) {
  return String(left || '').toLowerCase() === String(right || '').toLowerCase();
}

function deviceTopics(device, config) {
  const id = normalizeDeviceId(device);
  const isLegacy = sameDeviceId(id, 'light');
  const supplied = device && typeof device === 'object' ? device : {};
  const suppliedTopics = supplied.topics || {};
  return {
    command: isLegacy
      ? supplied.commandTopic || suppliedTopics.command || config.topics.command
      : `shuangshou/control/${id}`,
    state: isLegacy
      ? supplied.stateTopic || suppliedTopics.state || config.topics.state
      : `shuangshou/status/${id}`,
    availability: isLegacy
      ? supplied.availabilityTopic || suppliedTopics.availability || config.topics.availability || ''
      : `shuangshou/availability/${id}`
  };
}

function makeDeviceDescriptor(device, config) {
  const id = normalizeDeviceId(device);
  if (!id || !DEVICE_ID_PATTERN.test(id)) return null;
  const topics = deviceTopics(device, config);
  return {
    id,
    name: device && typeof device === 'object' ? String(device.name || id) : id,
    commandTopic: topics.command,
    stateTopic: topics.state,
    availabilityTopic: topics.availability,
    commandFormat: device && typeof device === 'object' && device.commandFormat === 'json'
      ? 'json'
      : config.commandFormat === 'json' ? 'json' : 'text'
  };
}

function parsePayload(payload) {
  const body = payload && typeof payload.toString === 'function' ? payload.toString() : String(payload || '');
  const text = body.trim();
  const action = normalizeAction(text);
  if (action) return { kind: 'state', state: action, raw: text };
  if (text.toUpperCase() === 'ONLINE') return { kind: 'availability', availability: 'online', raw: text };
  if (text.toUpperCase() === 'OFFLINE') return { kind: 'availability', availability: 'offline', raw: text };
  try {
    const data = JSON.parse(text);
    const state = normalizeAction(data && (data.state || data.relayState));
    if (state) return { kind: 'state', state, raw: text };
    if (data && typeof data.online === 'boolean') {
      return { kind: 'availability', availability: data.online ? 'online' : 'offline', raw: text };
    }
    const availability = String(data && (data.availability || data.status) || '').trim().toUpperCase();
    if (availability === 'ONLINE' || availability === 'OFFLINE') {
      return { kind: 'availability', availability: availability.toLowerCase(), raw: text };
    }
  } catch (error) {
    return null;
  }
  return null;
}

function createEmptyDeviceStatus(descriptor) {
  return {
    id: descriptor.id,
    relayState: null,
    state: null,
    stateKnown: false,
    pending: null,
    pendingAction: null,
    pendingSince: 0,
    historical: true,
    availability: 'unknown',
    online: null,
    availabilityHistorical: true,
    lastHeartbeatAt: 0,
    lastSeenAt: 0,
    message: '尚无设备回报'
  };
}

function createMqttService(options = {}) {
  const optionConfig = options.config || {};
  const config = {
    ...MQTT_CONFIG,
    ...optionConfig,
    topics: { ...MQTT_CONFIG.topics, ...(optionConfig.topics || {}) }
  };
  const listeners = new Set();
  const descriptors = new Map();
  const deviceStatuses = new Map();
  const pendingTimers = new Map();
  const availabilityTimers = new Map();
  const subscribedTopics = new Set();
  let client = null;
  let status = {
    connected: false,
    configured: Boolean(config.url),
    message: config.url ? '未连接' : 'MQTT 未配置'
  };

  function descriptorKey(id) {
    return normalizeDeviceId(id).toLowerCase();
  }

  function getDescriptor(device) {
    const id = normalizeDeviceId(device);
    if (!id) return null;
    const key = descriptorKey(id);
    return descriptors.get(key) || makeDeviceDescriptor(device, config);
  }

  function ensureDescriptor(device) {
    const descriptor = getDescriptor(device);
    if (!descriptor) return null;
    const key = descriptorKey(descriptor.id);
    if (!descriptors.has(key)) {
      descriptors.set(key, descriptor);
      if (client && status.connected) subscribeDescriptor(descriptor);
    }
    if (!deviceStatuses.has(key)) deviceStatuses.set(key, createEmptyDeviceStatus(descriptor));
    return descriptors.get(key);
  }

  function snapshotDevices() {
    const result = {};
    deviceStatuses.forEach((deviceStatus) => {
      result[deviceStatus.id] = { ...deviceStatus };
    });
    return result;
  }

  function globalStatus() {
    const light = deviceStatuses.get(descriptorKey('light'));
    return {
      ...status,
      devices: snapshotDevices(),
      deviceStatuses: snapshotDevices(),
      relayState: light ? light.relayState : null,
      state: light ? light.relayState : null,
      pending: light ? light.pending : null,
      pendingAction: light ? light.pendingAction : null,
      historical: light ? light.historical : true
    };
  }

  function emit(patch = {}) {
    status = { ...status, ...patch };
    const snapshot = globalStatus();
    listeners.forEach((listener) => listener(snapshot));
  }

  function subscribe(listener) {
    listeners.add(listener);
    listener(globalStatus());
    return () => listeners.delete(listener);
  }

  function setDeviceStatus(device, patch, globalPatch) {
    const descriptor = ensureDescriptor(device);
    if (!descriptor) return null;
    const state = deviceStatuses.get(descriptorKey(descriptor.id));
    Object.assign(state, patch);
    emit(globalPatch || {});
    return state;
  }

  function clearPending(descriptor, message) {
    const key = descriptorKey(descriptor.id);
    const timer = pendingTimers.get(key);
    if (timer) clearTimeout(timer);
    pendingTimers.delete(key);
    const state = deviceStatuses.get(key);
    if (!state) return;
    state.pending = null;
    state.pendingAction = null;
    state.pendingSince = 0;
    if (message) state.message = message;
  }

  function clearAvailabilityTimer(device) {
    const id = normalizeDeviceId(device);
    if (!id) return;
    const key = descriptorKey(id);
    const timer = availabilityTimers.get(key);
    if (timer) clearTimeout(timer);
    availabilityTimers.delete(key);
  }

  function clearAvailabilityTimers() {
    availabilityTimers.forEach((timer) => clearTimeout(timer));
    availabilityTimers.clear();
  }

  function getHeartbeatTimeoutMs() {
    const value = Number.isFinite(Number(options.heartbeatTimeoutMs))
      ? Number(options.heartbeatTimeoutMs)
      : Number.isFinite(Number(config.heartbeatTimeoutMs)) ? Number(config.heartbeatTimeoutMs) : 45000;
    return Math.max(0, value);
  }

  function scheduleAvailabilityExpiry(descriptor, heartbeatAt) {
    const key = descriptorKey(descriptor.id);
    clearAvailabilityTimer(descriptor);
    const timeoutMs = getHeartbeatTimeoutMs();
    if (!timeoutMs) return;
    availabilityTimers.set(key, setTimeout(() => {
      const current = deviceStatuses.get(key);
      if (!current || current.lastHeartbeatAt !== heartbeatAt || current.availability !== 'online') return;
      current.availability = 'unknown';
      current.online = null;
      current.availabilityHistorical = true;
      current.message = '心跳超时，设备状态未知';
      availabilityTimers.delete(key);
      emit({});
    }, timeoutMs));
  }

  function markLive(descriptor, now, message) {
    const key = descriptorKey(descriptor.id);
    const state = deviceStatuses.get(key);
    if (!state) return;
    state.availability = 'online';
    state.online = true;
    state.availabilityHistorical = false;
    state.lastHeartbeatAt = now;
    state.lastSeenAt = now;
    state.message = message || '设备在线心跳';
    scheduleAvailabilityExpiry(descriptor, now);
  }

  function subscribeTopic(topic) {
    if (!client || !status.connected || !topic || !client.subscribe || subscribedTopics.has(topic)) return;
    subscribedTopics.add(topic);
    client.subscribe(topic, (error, granted) => {
      if (error || (granted || []).some((item) => item && item.qos === 128)) {
        emit({ message: `设备状态订阅失败：${topic}` });
      }
    });
  }

  function subscribeDescriptor(descriptor) {
    subscribeTopic(descriptor.stateTopic);
    subscribeTopic(descriptor.availabilityTopic);
  }

  function registerDevice(device) {
    const nextDescriptor = makeDeviceDescriptor(device, config);
    if (!nextDescriptor) return null;
    const key = descriptorKey(nextDescriptor.id);
    const previous = descriptors.get(key);
    const previousTopics = previous
      ? [previous.stateTopic, previous.availabilityTopic].filter(Boolean)
      : [];
    const nextTopics = [nextDescriptor.stateTopic, nextDescriptor.availabilityTopic].filter(Boolean);
    const changedTopics = previous && previousTopics.some((topic) => !nextTopics.includes(topic));
    if (changedTopics) {
      const otherTopics = new Set();
      descriptors.forEach((descriptor, descriptorKeyValue) => {
        if (descriptorKeyValue === key) return;
        [descriptor.stateTopic, descriptor.availabilityTopic].filter(Boolean).forEach((topic) => otherTopics.add(topic));
      });
      const removeTopics = previousTopics.filter((topic) => !otherTopics.has(topic));
      if (client && status.connected && client.unsubscribe && removeTopics.length) client.unsubscribe(removeTopics);
      removeTopics.forEach((topic) => subscribedTopics.delete(topic));
    }
    descriptors.set(key, nextDescriptor);
    if (!deviceStatuses.has(key)) deviceStatuses.set(key, createEmptyDeviceStatus(nextDescriptor));
    else deviceStatuses.get(key).id = nextDescriptor.id;
    const descriptor = nextDescriptor;
    if (client && status.connected) subscribeDescriptor(descriptor);
    emit({});
    return clone(descriptor);
  }

  function registerDevices(devices) {
    (Array.isArray(devices) ? devices : []).forEach(registerDevice);
    return getDeviceStatuses();
  }

  function unregisterDevice(device) {
    const id = normalizeDeviceId(device);
    const key = descriptorKey(id);
    const descriptor = descriptors.get(key);
    if (!descriptor) return false;
    const topics = [descriptor.stateTopic, descriptor.availabilityTopic].filter(Boolean);
    const otherTopics = new Set();
    descriptors.forEach((otherDescriptor, otherKey) => {
      if (otherKey === key) return;
      [otherDescriptor.stateTopic, otherDescriptor.availabilityTopic]
        .filter(Boolean)
        .forEach((topic) => otherTopics.add(topic));
    });
    const releaseTopics = topics.filter((topic) => !otherTopics.has(topic));
    if (client && status.connected && client.unsubscribe && releaseTopics.length) client.unsubscribe(releaseTopics);
    releaseTopics.forEach((topic) => subscribedTopics.delete(topic));
    const timer = pendingTimers.get(key);
    if (timer) clearTimeout(timer);
    pendingTimers.delete(key);
    clearAvailabilityTimer(id);
    descriptors.delete(key);
    deviceStatuses.delete(key);
    emit({});
    return true;
  }

  function handleMessage(topic, payload, packet) {
    const descriptor = [...descriptors.values()].find((item) => item.stateTopic === topic || item.availabilityTopic === topic);
    if (!descriptor) return;
    const data = parsePayload(payload);
    if (!data) return;
    const retained = Boolean(packet && packet.retain);
    const state = deviceStatuses.get(descriptorKey(descriptor.id));
    if (!state) return;
    const now = Date.now();
    if (data.kind === 'availability') {
      if (retained) {
        // retained availability 是 broker 的历史遗留值，不能证明此刻在线；
        // 若此前已有实时心跳，则不降级已确认的实时在线状态。
        if (!(state.availability === 'online' && state.online === true && !state.availabilityHistorical)) {
          state.availability = 'unknown';
          state.online = null;
          state.availabilityHistorical = true;
        }
        state.message = '收到历史可用性，不代表当前在线';
        emit({});
        return;
      }
      if (data.availability === 'online') {
        markLive(descriptor, now, '设备在线心跳');
      } else {
        clearAvailabilityTimer(descriptor);
        state.availability = 'offline';
        state.online = false;
        state.availabilityHistorical = false;
        state.lastHeartbeatAt = now;
        state.lastSeenAt = now;
        state.message = '设备已离线';
      }
      emit({});
      return;
    }

    state.relayState = data.state;
    state.state = data.state;
    state.stateKnown = true;
    state.historical = retained;
    state.lastSeenAt = now;
    if (!retained) {
      markLive(descriptor, now, '已收到设备状态回报');
    }
    if (!retained && state.pending === data.state) {
      clearPending(descriptor, '已收到设备状态回报');
    } else {
      state.message = retained ? '收到历史状态，不代表当前在线' : '已收到设备状态回报';
    }
    emit({});
  }

  function connect() {
    if (client) return Promise.resolve(globalStatus());
    if (!config.url) {
      emit({ connected: false, message: '请在 services/mqtt.js 配置 MQTT Broker' });
      return Promise.resolve(globalStatus());
    }
    let mqtt;
    try {
      mqtt = options.mqtt || require('../vendor/mqtt.min');
    } catch (error) {
      emit({ connected: false, message: 'MQTT 客户端库未配置' });
      return Promise.resolve(globalStatus());
    }
    emit({ message: '正在连接 MQTT' });
    try {
      client = mqtt.connect(config.url, {
        clientId: config.clientId || `shuangshou_${Date.now()}`,
        username: config.username,
        password: config.password,
        reconnectPeriod: 3000,
        connectTimeout: 10000,
        clean: true
      });
      client.on('connect', () => {
        emit({ connected: true, message: 'MQTT 已连接' });
        descriptors.forEach(subscribeDescriptor);
      });
      client.on('message', handleMessage);
      client.on('error', () => emit({ connected: false, message: 'MQTT 连接错误' }));
      client.on('close', () => {
        subscribedTopics.clear();
        clearAvailabilityTimers();
        deviceStatuses.forEach((deviceStatus) => {
          clearPending(descriptors.get(descriptorKey(deviceStatus.id)), 'MQTT 已断开，等待设备状态');
          deviceStatus.historical = true;
          deviceStatus.availability = 'unknown';
          deviceStatus.online = null;
          deviceStatus.availabilityHistorical = true;
        });
        emit({ connected: false, message: 'MQTT 已断开，正在重连' });
      });
    } catch (error) {
      client = null;
      emit({ connected: false, message: '连接失败，请检查网络及 socket 合法域名' });
    }
    return Promise.resolve(globalStatus());
  }

  function publish(device, value) {
    const descriptor = ensureDescriptor(device);
    const action = normalizeAction(value);
    if (!descriptor || !action) return false;
    const state = deviceStatuses.get(descriptorKey(descriptor.id));
    if (state.pending) return false;
    if (!client || !status.connected) {
      state.message = 'MQTT 未连接，未发送指令';
      emit({ message: 'MQTT 未连接，未发送指令' });
      return false;
    }

    clearPending(descriptor);
    state.pending = action;
    state.pendingAction = action;
    state.pendingSince = Date.now();
    state.message = '正在等待继电器回报';
    emit({ message: `正在等待${descriptor.name}回报` });
    const key = descriptorKey(descriptor.id);
    const timeoutMs = Number.isFinite(Number(options.pendingTimeoutMs))
      ? Number(options.pendingTimeoutMs)
      : Number.isFinite(Number(config.pendingTimeoutMs)) ? Number(config.pendingTimeoutMs) : 8000;
    pendingTimers.set(key, setTimeout(() => {
      const current = deviceStatuses.get(key);
      if (!current || current.pending !== action) return;
      current.pending = null;
      current.pendingAction = null;
      current.pendingSince = 0;
      current.message = '未收到设备回报，请检查 ESP 电源及 Wi-Fi';
      pendingTimers.delete(key);
      emit({ message: current.message });
    }, timeoutMs));

    const payload = descriptor.commandFormat === 'json' ? JSON.stringify({ action }) : action;
    try {
      client.publish(descriptor.commandTopic, payload, { qos: 0, retain: false }, (error) => {
        if (!error) return;
        clearPending(descriptor, '指令发送失败');
        emit({ message: '指令发送失败' });
      });
    } catch (error) {
      clearPending(descriptor, '指令发送失败');
      emit({ message: '指令发送失败' });
      return false;
    }
    return true;
  }

  function getDeviceStatus(device) {
    const id = normalizeDeviceId(device);
    if (!id) return null;
    const state = deviceStatuses.get(descriptorKey(id));
    return state ? clone(state) : null;
  }

  function getDeviceStatuses() {
    return clone(snapshotDevices());
  }

  function getStatus(device) {
    if (device !== undefined && device !== null) {
      const deviceStatus = getDeviceStatus(device);
      return deviceStatus ? { ...globalStatus(), ...deviceStatus } : null;
    }
    return globalStatus();
  }

  function disconnect() {
    pendingTimers.forEach((timer) => clearTimeout(timer));
    pendingTimers.clear();
    clearAvailabilityTimers();
    deviceStatuses.forEach((deviceStatus) => {
      deviceStatus.pending = null;
      deviceStatus.pendingAction = null;
      deviceStatus.pendingSince = 0;
      deviceStatus.historical = true;
      deviceStatus.availability = 'unknown';
      deviceStatus.online = null;
      deviceStatus.availabilityHistorical = true;
    });
    subscribedTopics.clear();
    if (client) {
      if (client.removeAllListeners) client.removeAllListeners();
      if (client.end) client.end();
    }
    client = null;
    emit({ connected: false, message: 'MQTT 已断开' });
  }

  ensureDescriptor({
    id: 'light',
    name: '灯',
    commandTopic: config.topics.command,
    stateTopic: config.topics.state,
    availabilityTopic: config.topics.availability,
    commandFormat: config.commandFormat
  });

  return {
    config,
    subscribe,
    connect,
    registerDevice,
    registerDevices,
    unregisterDevice,
    publish,
    disconnect,
    getStatus,
    getDeviceStatus,
    getDeviceStatuses,
    parsePayload
  };
}

module.exports = { MQTT_CONFIG, createMqttService, parsePayload, normalizeAction };
