const JDY23_SERVICE_SHORT_UUID = 'FFE0';
const JDY23_CHARACTERISTIC_SHORT_UUID = 'FFE1';
const JDY23_CONNECT_ATTEMPTS = 3;

function errorText(error) {
  if (!error) return '未知蓝牙错误';
  return error.errMsg || error.message || String(error);
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function promisifyWx(apiName, options = {}) {
  return new Promise((resolve, reject) => {
    if (typeof wx === 'undefined' || typeof wx[apiName] !== 'function') {
      reject(new Error(`当前环境不支持 wx.${apiName}`));
      return;
    }
    wx[apiName]({ ...options, success: resolve, fail: reject });
  });
}

function shortUuid(uuid) {
  const normalized = String(uuid || '').replace(/-/g, '').toUpperCase();
  if (/^0000[0-9A-F]{4}00001000800000805F9B34FB$/.test(normalized)) {
    return normalized.slice(4, 8);
  }
  return normalized;
}

function sameUuid(uuid, expectedShortUuid) {
  return shortUuid(uuid) === expectedShortUuid;
}

function encodeUtf8(text) {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(text).buffer;
  const encoded = unescape(encodeURIComponent(text));
  const bytes = new Uint8Array(encoded.length);
  for (let index = 0; index < encoded.length; index += 1) bytes[index] = encoded.charCodeAt(index);
  return bytes.buffer;
}

function createBluetoothClient(callbacks = {}) {
  const onStateChange = callbacks.onStateChange || function noop() {};
  const onDeviceFound = callbacks.onDeviceFound || function noop() {};
  const onValueChange = callbacks.onValueChange || function noop() {};
  const onError = callbacks.onError || function noop() {};
  let activeDevice = null;
  let activeDeviceId = '';
  let activeServiceId = '';
  let activeCharacteristicId = '';
  let writeCharacteristicId = '';
  let adapterReady = false;
  let notifyEnabled = false;
  let manualDisconnect = false;
  let eventHandlersBound = false;
  let connectPromise = null;
  let reconnectTimer = null;
  let reconnectRound = 0;
  let suppressReconnectUntil = 0;

  function emitState(patch = {}) {
    onStateChange({
      adapterReady,
      deviceId: activeDeviceId,
      deviceName: activeDevice ? (activeDevice.localName || activeDevice.name || '') : '',
      serviceId: activeServiceId,
      characteristicId: activeCharacteristicId,
      notifyEnabled,
      ...patch
    });
  }

  function clearReconnectTimer() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  }

  function clearLink() {
    activeServiceId = '';
    activeCharacteristicId = '';
    writeCharacteristicId = '';
    notifyEnabled = false;
  }

  function bindEventHandlers() {
    if (eventHandlersBound || typeof wx === 'undefined') return;
    eventHandlersBound = true;

    wx.onBluetoothAdapterStateChange((state) => {
      adapterReady = Boolean(state.available);
      emitState({
        available: Boolean(state.available),
        discovering: Boolean(state.discovering),
        message: state.available ? undefined : '系统蓝牙不可用，请打开手机蓝牙后重试。'
      });
    });

    wx.onBluetoothDeviceFound((result) => {
      (result.devices || []).forEach((device) => {
        if (device && device.deviceId) onDeviceFound(device);
      });
    });

    wx.onBLECharacteristicValueChange((result) => {
      if (!result || !result.value || result.deviceId !== activeDeviceId) return;
      if (activeServiceId && result.serviceId && !sameUuid(result.serviceId, shortUuid(activeServiceId))) return;
      if (activeCharacteristicId && result.characteristicId && !sameUuid(result.characteristicId, shortUuid(activeCharacteristicId))) return;
      onValueChange(result.value, result);
    });

    wx.onBLEConnectionStateChange((result) => {
      if (!result || result.deviceId !== activeDeviceId || result.connected) return;
      const suppressReconnect = Date.now() < suppressReconnectUntil;
      clearLink();
      emitState({ connected: false, connecting: false, discovering: false, message: '设备连接已断开。' });
      if (!manualDisconnect && !suppressReconnect) scheduleReconnect();
    });
  }

  async function initAdapter() {
    if (adapterReady) {
      bindEventHandlers();
      return;
    }
    await promisifyWx('openBluetoothAdapter');
    adapterReady = true;
    bindEventHandlers();
    emitState({ available: true, message: '蓝牙已就绪，可搜索 JDY-23。' });
  }

  async function stopDiscovery() {
    try {
      await promisifyWx('stopBluetoothDevicesDiscovery');
    } catch (error) {
      // Scanning may already be stopped, which already meets the end state.
    }
    emitState({ discovering: false });
  }

  async function startDiscovery() {
    await initAdapter();
    await stopDiscovery();
    await promisifyWx('startBluetoothDevicesDiscovery', { allowDuplicatesKey: false });
    emitState({ discovering: true, message: '正在搜索附近的 JDY-23…' });
  }

  async function discoverJdy23Endpoint(deviceId) {
    let services = [];
    for (let attempt = 1; attempt <= 3; attempt += 1) {
      const result = await promisifyWx('getBLEDeviceServices', { deviceId });
      services = result.services || [];
      const targetService = services.find((item) => sameUuid(item.uuid, JDY23_SERVICE_SHORT_UUID));
      if (targetService) {
        const charsResult = await promisifyWx('getBLEDeviceCharacteristics', { deviceId, serviceId: targetService.uuid });
        const characteristics = charsResult.characteristics || [];
        const characteristic = characteristics.find((item) => sameUuid(item.uuid, JDY23_CHARACTERISTIC_SHORT_UUID));
        if (!characteristic) {
          const available = characteristics.map((item) => shortUuid(item.uuid)).join(', ') || '无';
          throw new Error(`已找到 FFE0，但未找到 FFE1（可用特征：${available}）。`);
        }
        const props = characteristic.properties || {};
        if (!props.notify && !props.indicate) throw new Error('FFE1 不支持 Notify，无法接收 STM32 数据。');
        if (!props.write && !props.writeNoResponse) throw new Error('FFE1 不支持 Write，无法发送控制命令。');
        return { serviceId: targetService.uuid, characteristicId: characteristic.uuid, writeCharacteristicId: characteristic.uuid };
      }
      if (attempt < 3) await delay(220 * attempt);
    }
    const available = services.map((item) => shortUuid(item.uuid)).join(', ') || '无';
    throw new Error(`未发现 JDY-23 的 FFE0 服务（可用服务：${available}）。`);
  }

  async function connectOnce() {
    await promisifyWx('createBLEConnection', { deviceId: activeDeviceId, timeout: 10000 });
    emitState({ connecting: true, message: '已连接设备，正在发现 FFE0 / FFE1…' });
    const endpoint = await discoverJdy23Endpoint(activeDeviceId);
    activeServiceId = endpoint.serviceId;
    activeCharacteristicId = endpoint.characteristicId;
    writeCharacteristicId = endpoint.writeCharacteristicId;
    emitState({ connecting: true, message: '已发现 FFE0 / FFE1，正在开启数据订阅…' });
    await promisifyWx('notifyBLECharacteristicValueChange', {
      deviceId: activeDeviceId,
      serviceId: activeServiceId,
      characteristicId: activeCharacteristicId,
      state: true
    });
    notifyEnabled = true;
  }

  function scheduleReconnect() {
    clearReconnectTimer();
    if (!activeDevice || manualDisconnect || reconnectRound >= 3) {
      if (reconnectRound >= 3) emitState({ reconnecting: false, message: '自动重连已停止，请手动重新连接设备。' });
      return;
    }
    reconnectRound += 1;
    const waitMs = 700 * reconnectRound;
    emitState({ reconnecting: true, message: `连接断开，${Math.ceil(waitMs / 1000)} 秒后第 ${reconnectRound} 次重连…` });
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect(activeDevice, { reconnect: true }).catch(() => scheduleReconnect());
    }, waitMs);
  }

  function connect(device, options = {}) {
    if (!device || !device.deviceId) return Promise.reject(new Error('缺少 BLE 设备标识。'));
    if (connectPromise) return connectPromise;
    if (activeDeviceId === device.deviceId && notifyEnabled) {
      emitState({ connected: true, connecting: false, reconnecting: false, message: 'JDY-23 已连接并正在接收数据。' });
      return Promise.resolve();
    }

    activeDevice = device;
    activeDeviceId = device.deviceId;
    manualDisconnect = false;
    suppressReconnectUntil = 0;
    clearReconnectTimer();
    clearLink();
    connectPromise = (async () => {
      await stopDiscovery();
      emitState({ connecting: true, connected: false, reconnecting: Boolean(options.reconnect), message: '正在连接 JDY-23…' });
      const failures = [];
      for (let attempt = 1; attempt <= JDY23_CONNECT_ATTEMPTS; attempt += 1) {
        try {
          await connectOnce();
          reconnectRound = 0;
          suppressReconnectUntil = 0;
          emitState({ connected: true, connecting: false, reconnecting: false, message: 'JDY-23 已连接，FFE1 通知已开启。' });
          return;
        } catch (error) {
          failures.push(`第 ${attempt} 次：${errorText(error)}`);
          clearLink();
          suppressReconnectUntil = Date.now() + 2000;
          try {
            await promisifyWx('closeBLEConnection', { deviceId: activeDeviceId });
          } catch (closeError) {
            // The attempted link may never have reached an open state.
          }
          if (attempt < JDY23_CONNECT_ATTEMPTS) await delay(260 * attempt);
        }
      }
      const message = `JDY-23 建链失败（${failures.join('；')}）`;
      emitState({ connected: false, connecting: false, reconnecting: false, message });
      throw new Error(message);
    })();
    return connectPromise.finally(() => { connectPromise = null; });
  }

  async function disconnect() {
    manualDisconnect = true;
    clearReconnectTimer();
    const deviceId = activeDeviceId;
    clearLink();
    if (deviceId) {
      try {
        await promisifyWx('closeBLEConnection', { deviceId });
      } catch (error) {
        // A closed connection is equivalent to the requested end state.
      }
    }
    activeDevice = null;
    activeDeviceId = '';
    reconnectRound = 0;
    emitState({ connected: false, connecting: false, reconnecting: false, message: '已主动断开 JDY-23。' });
  }

  async function closeAdapter() {
    await disconnect();
    try {
      await promisifyWx('closeBluetoothAdapter');
    } catch (error) {
      onError(error);
    }
    adapterReady = false;
    emitState({ available: false, discovering: false, message: '蓝牙适配器已关闭。' });
  }

  function write(arrayBuffer) {
    if (!activeDeviceId || !activeServiceId || !writeCharacteristicId || !notifyEnabled) {
      return Promise.reject(new Error('当前 FFE1 写通道未就绪。'));
    }
    return promisifyWx('writeBLECharacteristicValue', {
      deviceId: activeDeviceId,
      serviceId: activeServiceId,
      characteristicId: writeCharacteristicId,
      value: arrayBuffer
    });
  }

  return {
    initAdapter,
    startDiscovery,
    stopDiscovery,
    connect,
    disconnect,
    closeAdapter,
    write,
    writeText: (text) => write(encodeUtf8(`${String(text).replace(/\r?\n$/, '')}\r\n`)),
    getDebugState: () => ({ activeDeviceId, activeServiceId, activeCharacteristicId, notifyEnabled, adapterReady })
  };
}

module.exports = {
  JDY23_SERVICE_SHORT_UUID,
  JDY23_CHARACTERISTIC_SHORT_UUID,
  createBluetoothClient,
  promisifyWx,
  shortUuid
};
