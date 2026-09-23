const { createMqttService } = require('../../services/mqtt');
const { createGestureStore } = require('../../services/gesture-store');
const { createDeviceStore } = require('../../services/device-store');
const { createRelayGestureGate, normalizeRelayAction } = require('../../services/relay-gesture');

function findStatus(statuses, id) {
  if (!statuses || !id) return null;
  if (statuses[id]) return statuses[id];
  const key = String(id).toLowerCase();
  const actualId = Object.keys(statuses).find((item) => item.toLowerCase() === key);
  return actualId ? statuses[actualId] : null;
}

function formatRelayState(state) {
  if (!state || !state.stateKnown) return '状态未知';
  const text = state.relayState === 'ON' ? '已闭合' : state.relayState === 'OFF' ? '已断开' : '状态未知';
  if (state.historical) return `${text}（历史记录，不代表当前在线）`;
  return `${text}（设备回报）`;
}

function formatAvailability(state) {
  if (!state
    || state.availability === 'unknown'
    || state.online === null
    || state.availabilityHistorical === true) return '设备状态未知';
  if (state.availability === 'online' && state.online === true) return '设备在线（已收到回报）';
  return '设备已离线';
}

function toast(message) {
  if (typeof wx !== 'undefined' && wx.showToast) wx.showToast({ title: String(message), icon: 'none' });
}

Page({
  data: {
    connected: false,
    gestureControl: false,
    gestureMessage: '打开开关后，用已绑定的 ON / OFF 控制手势操作',
    fingers: [],
    enabledFingers: [],
    statusText: '蓝牙未连接',
    mqttConnected: false,
    mqttMessage: '尚未连接',
    pending: false,
    lastState: '状态未知',
    selectedDeviceId: 'light',
    selectedDeviceName: '灯',
    selectedAvailability: '设备状态未知',
    devices: [],
    devicePickerNames: ['兼容 legacy 灯', '跟随当前选择'],
    controlGestures: [],
    safetyAlert: null,
    buzzerStatus: '',
    showDeviceEditor: false,
    editingDeviceId: '',
    deviceNameDraft: '',
    deviceIdDraft: '',
    deviceEditorError: ''
  },

  onLoad() {
    this.runtime = getApp().getRuntime();
    this.mqtt = createMqttService();
    this.gestureStore = createGestureStore();
    this.deviceStore = createDeviceStore();
    this.gate = createRelayGestureGate();
    this.active = false;
    this.controlGestures = [];
    this.gestureBindings = [];
    this.refreshDevices();
    this.unsubscribeRuntime = this.runtime.subscribe((state) => {
      if (!this.active) return;
      this.evaluateGesture(state);
      this.setData({
        connected: state.connected,
        fingers: state.flex,
        enabledFingers: state.calibration.enabledFingers,
        statusText: state.statusText,
        safetyAlert: state.safetyAlert,
        buzzerStatus: state.buzzerStatus
      });
    });
    this.unsubscribeMqtt = this.mqtt.subscribe((status) => {
      if (!this.active) return;
      this.applyMqttStatus(status);
    });
  },

  onShow() {
    this.active = true;
    this.refreshDevices();
    this.applyMqttStatus(this.mqtt.getStatus());
    this.runtime.setMode(this.runtime.modes.remote);
    const state = this.runtime.getState();
    this.setData({
      connected: state.connected,
      fingers: state.flex,
      enabledFingers: state.calibration.enabledFingers,
      statusText: state.statusText,
      safetyAlert: state.safetyAlert,
      buzzerStatus: state.buzzerStatus
    });
  },

  onHide() {
    this.active = false;
    this.gate.reset();
    this.setData({ gestureControl: false });
  },

  onUnload() {
    this.onHide();
    this.mqtt.disconnect();
    if (this.unsubscribeRuntime) this.unsubscribeRuntime();
    if (this.unsubscribeMqtt) this.unsubscribeMqtt();
  },

  refreshDevices() {
    const devices = this.deviceStore.list();
    const selectedDeviceId = this.deviceStore.selectedId();
    this.mqtt.registerDevices(devices);
    this.controlGestures = this.gestureStore.list().filter((gesture) => gesture.category === 'control' && gesture.enabled !== false);
    this.gestureBindings = this.deviceStore.listBindings();
    const devicePickerNames = [
      '兼容 legacy 灯',
      '跟随当前选择',
      ...devices.map((device) => `${device.name}（${device.id}）`)
    ];
    const controlGestures = this.controlGestures.map((gesture) => {
      const binding = this.gestureBindings.find((item) => item.gestureId === gesture.id);
      const boundDevice = binding && devices.find((device) => device.id.toLowerCase() === String(binding.deviceId).toLowerCase());
      const bindingDisabled = Boolean(binding && (binding.disabled === true || binding.mode === 'disabled'));
      const followsSelected = Boolean(binding
        && (binding.mode === 'follow-selected' || binding.followSelected === true));
      return {
        ...gesture,
        action: normalizeRelayAction(gesture.action || gesture.text) || gesture.action || gesture.text || '未设置',
        bindingIndex: boundDevice && !bindingDisabled
          ? devices.findIndex((device) => device.id === boundDevice.id) + 2
          : followsSelected ? 1 : 0,
        bindingDeviceId: boundDevice && !bindingDisabled ? boundDevice.id : '',
        bindingName: bindingDisabled
          ? '已禁用：原设备已删除'
          : followsSelected ? '跟随当前选择' : boundDevice ? boundDevice.name : '兼容 legacy 灯'
      };
    });
    this.setData({
      devices: this.decorateDevices(this.mqtt.getDeviceStatuses(), devices, selectedDeviceId),
      selectedDeviceId: selectedDeviceId || '',
      selectedDeviceName: (devices.find((device) => device.id === selectedDeviceId) || {}).name || '未选择设备',
      devicePickerNames,
      controlGestures
    });
  },

  decorateDevices(statuses, devices, selectedDeviceId) {
    return devices.map((device) => {
      const state = findStatus(statuses, device.id) || {
        stateKnown: false,
        relayState: null,
        pending: null,
        historical: true,
        availability: 'unknown',
        online: null,
        availabilityHistorical: true
      };
      return {
        ...device,
        selected: device.id === selectedDeviceId,
        relayState: state.relayState,
        stateKnown: Boolean(state.stateKnown),
        pending: Boolean(state.pending),
        stateText: formatRelayState(state),
        availability: state.availability,
        availabilityText: formatAvailability(state),
        actionText: state.pending ? `等待 ${state.pending}` : '控制'
      };
    });
  },

  handleMqttConnect() {
    if (this.mqtt.getStatus().connected) this.mqtt.disconnect();
    else this.mqtt.connect();
  },

  handleSelectDevice(event) {
    const id = event.currentTarget.dataset.deviceId || event.currentTarget.dataset.deviceid;
    if (!id) return;
    try {
      this.deviceStore.select(id);
      this.gate.reset();
      this.refreshDevices();
      this.applyMqttStatus(this.mqtt.getStatus());
    } catch (error) {
      toast(error.message || '选择设备失败');
    }
  },

  toggleGestureControl(event) {
    this.gate.reset();
    this.refreshDevices();
    if (!this.data.selectedDeviceId && event.detail.value) {
      toast('请先添加并选择设备');
      this.setData({ gestureControl: false });
      return;
    }
    this.setData({ gestureControl: Boolean(event.detail.value), gestureMessage: '等待控制手势' });
  },

  evaluateGesture(state) {
    if (!this.data.gestureControl) return;
    const mqtt = this.mqtt.getStatus();
    if (!mqtt.connected) {
      this.gate.reset();
      this.setData({ gestureMessage: '请先连接 MQTT' });
      return;
    }
    const result = this.gate.update(this.controlGestures, state, {
      selectedDeviceId: this.data.selectedDeviceId,
      bindings: this.gestureBindings,
      statuses: this.mqtt.getDeviceStatuses(),
      requireLastFlexAt: true
    });
    if (result.action && result.deviceId) {
      const published = this.mqtt.publish(result.deviceId, result.action.toLowerCase());
      const targetStatus = this.mqtt.getDeviceStatus(result.deviceId);
      if (!published && (!targetStatus || !targetStatus.pending)) {
        result.message = '设备指令未发送，请检查 MQTT 连接';
      }
    }
    if (result.message !== this.data.gestureMessage) this.setData({ gestureMessage: result.message });
  },

  applyMqttStatus(status) {
    const selectedId = this.deviceStore ? this.deviceStore.selectedId() : this.data.selectedDeviceId;
    const devices = this.deviceStore ? this.deviceStore.list() : [];
    const selected = findStatus(status.devices || status.deviceStatuses, selectedId);
    const lastState = formatRelayState(selected);
    this.setData({
      devices: this.decorateDevices(status.devices || status.deviceStatuses, devices, selectedId),
      selectedDeviceId: selectedId || '',
      selectedDeviceName: (devices.find((device) => device.id === selectedId) || {}).name || '未选择设备',
      selectedAvailability: formatAvailability(selected),
      mqttConnected: status.connected,
      mqttMessage: status.message,
      pending: Boolean(selected && selected.pending),
      lastState
    });
  },

  handleDeviceToggle(event) {
    const id = event.currentTarget.dataset.deviceId || this.data.selectedDeviceId;
    const value = event.currentTarget.dataset.value;
    if (!id || !value) return;
    if (!this.mqtt.publish(id, value)) toast('指令未发送：请检查 MQTT 连接或等待当前设备回报');
  },

  openAddDevice() {
    this.setData({
      showDeviceEditor: true,
      editingDeviceId: '',
      deviceNameDraft: '',
      deviceIdDraft: '',
      deviceEditorError: ''
    });
  },

  openEditDevice(event) {
    const id = event.currentTarget.dataset.deviceId || event.currentTarget.dataset.deviceid;
    const device = this.deviceStore.get(id);
    if (!device) return;
    this.setData({
      showDeviceEditor: true,
      editingDeviceId: device.id,
      deviceNameDraft: device.name,
      deviceIdDraft: device.id,
      deviceEditorError: ''
    });
  },

  handleDeviceNameInput(event) {
    this.setData({ deviceNameDraft: event.detail.value });
  },

  handleDeviceIdInput(event) {
    this.setData({ deviceIdDraft: event.detail.value });
  },

  cancelDeviceEditor() {
    this.setData({ showDeviceEditor: false, deviceEditorError: '' });
  },

  saveDevice() {
    const name = String(this.data.deviceNameDraft || '').trim();
    const id = String(this.data.deviceIdDraft || '').trim();
    if (!name || !id) {
      this.setData({ deviceEditorError: '请填写设备名称和 ESP-01 唯一 ID。' });
      return;
    }
    try {
      const oldId = this.data.editingDeviceId;
      const device = this.data.editingDeviceId
        ? this.deviceStore.update(this.data.editingDeviceId, { name, id })
        : this.deviceStore.add({ name, id });
      if (oldId && oldId.toLowerCase() !== device.id.toLowerCase()) this.mqtt.unregisterDevice(oldId);
      this.deviceStore.select(device.id);
      this.mqtt.registerDevice(device);
      this.gate.reset();
      this.setData({ showDeviceEditor: false, deviceEditorError: '' });
      this.refreshDevices();
      this.applyMqttStatus(this.mqtt.getStatus());
    } catch (error) {
      this.setData({ deviceEditorError: error.message || '保存设备失败' });
    }
  },

  handleDeleteDevice(event) {
    const id = event.currentTarget.dataset.deviceId || event.currentTarget.dataset.deviceid;
    const device = this.deviceStore.get(id);
    if (!device) return;
    const remove = () => {
      try {
        this.mqtt.unregisterDevice(device.id);
        this.deviceStore.remove(device.id);
        this.gate.reset();
        this.refreshDevices();
        this.applyMqttStatus(this.mqtt.getStatus());
      } catch (error) {
        toast(error.message || '删除设备失败');
      }
    };
    if (typeof wx !== 'undefined' && wx.showModal) {
      wx.showModal({
        title: '删除设备',
        content: `确定删除“${device.name}”吗？指向它的手势会保留为禁用状态，需显式重新绑定。`,
        success: (result) => { if (result.confirm) remove(); }
      });
    } else {
      remove();
    }
  },

  handleGestureBindingChange(event) {
    const gestureId = event.currentTarget.dataset.gestureId || event.currentTarget.dataset.gestureid;
    const index = Number(event.detail.value);
    const devices = this.deviceStore.list();
    try {
      const binding = this.deviceStore.getGestureBinding(gestureId);
      if (index === 0) {
        if (binding && (binding.disabled === true || binding.mode === 'disabled')) {
          toast('原设备已删除，请显式选择新设备或跟随当前选择');
          return;
        }
        this.deviceStore.unbindGesture(gestureId);
      } else if (index === 1) {
        const gesture = this.controlGestures.find((item) => item.id === gestureId);
        this.deviceStore.followSelectedGesture(gestureId, gesture && gesture.action);
      } else if (devices[index - 2]) {
        const gesture = this.controlGestures.find((item) => item.id === gestureId);
        this.deviceStore.bindGesture(gestureId, devices[index - 2].id, gesture && gesture.action);
      }
      this.gate.reset();
      this.refreshDevices();
    } catch (error) {
      toast(error.message || '手势绑定失败');
    }
  },

  clearGestureBinding(event) {
    const gestureId = event.currentTarget.dataset.gestureId || event.currentTarget.dataset.gestureid;
    this.deviceStore.unbindGesture(gestureId);
    this.gate.reset();
    this.refreshDevices();
  },

  handleAcknowledgeSafety() {
    this.runtime.acknowledgeSafety();
  }
});
