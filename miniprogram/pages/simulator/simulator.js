const { PRESETS } = require('../../services/simulator');

const FINGER_LABELS = ['左拇指', '左食指', '左中指', '左无名指', '左小指', '右拇指', '右食指', '右中指', '右无名指', '右小指'];
const PRESET_ORDER = ['all_straight', 'all_half', 'all_bent', 'mixed_fingers', 'normal_pose'];

function displaySnapshot(snapshot) {
  const source = snapshot || { flex: Array(10).fill(0), imu: {}, acc: {} };
  return {
    fingers: FINGER_LABELS.map((label, index) => ({ label, index, value: Number(source.flex[index] || 0) })),
    imu: {
      roll: Number(source.imu.roll || 0),
      pitch: Number(source.imu.pitch || 0),
      yaw: Number(source.imu.yaw || 0)
    },
    // slider 只支持正整数步进；ACC 用 0.1g 的整数刻度展示。
    acc: {
      x: Math.round(Number(source.acc.x || 0) * 10),
      y: Math.round(Number(source.acc.y || 0) * 10),
      z: Math.round(Number(source.acc.z || 0) * 10)
    }
  };
}

function simulatorTone(simulator) {
  return simulator && simulator.running ? 'success' : simulator && simulator.active ? 'info' : 'idle';
}

Page({
  data: {
    simulator: { active: false, running: false, lastInjectedAt: 0 },
    tone: 'idle',
    sourceLabel: '未启动',
    fingers: [],
    imu: { roll: 0, pitch: 0, yaw: 0 },
    acc: { x: 0, y: 0, z: 10 },
    presets: PRESET_ORDER.map((key) => ({ key, label: PRESETS[key].label }))
  },

  onLoad() {
    this.runtime = getApp().getRuntime();
    this.active = true;
    this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state));
  },

  onShow() {
    this.active = true;
    this.applyState(this.runtime.getState());
  },

  onHide() {
    this.active = false;
  },

  onUnload() {
    if (this.unsubscribe) this.unsubscribe();
  },

  applyState(state) {
    if (!this.active) return;
    const simulator = state.simulator || {};
    const display = displaySnapshot(simulator.snapshot);
    this.setData({
      simulator,
      tone: simulatorTone(simulator),
      sourceLabel: simulator.running ? '连续模拟中' : simulator.active ? '已注入单次测试数据' : '未启动',
      ...display
    });
  },

  updateFinger(event) {
    const index = Number(event.currentTarget.dataset.index);
    const value = Number(event.detail.value);
    const snapshot = this.runtime.getSimulator().getSnapshot();
    snapshot.flex[index] = value;
    this.runtime.setSimulatorSnapshot({ flex: snapshot.flex });
  },

  updateImu(event) {
    const axis = event.currentTarget.dataset.axis;
    this.runtime.setSimulatorSnapshot({ imu: { [axis]: Number(event.detail.value) } });
  },

  updateAcc(event) {
    const axis = event.currentTarget.dataset.axis;
    this.runtime.setSimulatorSnapshot({ acc: { [axis]: Number(event.detail.value) / 10 } });
  },

  applyPreset(event) {
    this.runtime.applySimulatorPreset(event.currentTarget.dataset.preset);
  },

  injectOnce() {
    this.runtime.injectSimulator({ includeStatus: true });
  },

  toggleContinuous() {
    if (this.data.simulator.running) this.runtime.stopSimulator();
    else this.runtime.startSimulator();
  }
});
