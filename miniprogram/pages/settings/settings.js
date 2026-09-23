const FINGER_LABELS = ['左拇指', '左食指', '左中指', '左无名指', '左小指', '右拇指', '右食指', '右中指', '右无名指', '右小指'];
const { formatMetric } = require('../../utils/realtime-view');
const JY_ERR_ANGLE_ZERO = 0x10;
function tone(state) { return state.connected ? 'success' : state.connecting || state.reconnecting ? 'warning' : state.lastError ? 'danger' : 'idle'; }
function formatNumber(value) { return formatMetric(value, '—'); }
function getJyStatus(jyStatus, bringup) {
  if (jyStatus && jyStatus.source === 'dynamic') {
    const online = jyStatus.online ? 1 : 0;
    const errorStreak = jyStatus.errorStreak === null ? '—' : jyStatus.errorStreak;
    const lastError = jyStatus.lastErrorMask === null ? '—' : jyStatus.lastErrorMask;
    const age = jyStatus.sampleAgeMs === null ? '—' : jyStatus.sampleAgeMs;
    const dynamicCopy = `JY|ONLINE=${online}|ERR=${errorStreak}|LAST=${lastError}|AGE=${age}ms`;
    if (!jyStatus.online) return { online: false, title: 'JY61P 传感器离线', detail: `${dynamicCopy}；ONLINE=0，当前不能确认传感器在线。` };
    if (jyStatus.stale) return { online: false, title: 'JY61P 数据过期', detail: `${dynamicCopy}；最近成功 ACC 样本年龄过大。` };
    if (jyStatus.lastErrorMask !== 0) {
      if ((jyStatus.lastErrorMask & JY_ERR_ANGLE_ZERO) !== 0) {
        return { online: false, title: 'JY61P 姿态帧异常', detail: `${dynamicCopy}；本次 Roll/Pitch/Yaw 原始值全零。页面仅展示最近一次有效姿态，报警保护已暂停。` };
      }
      return { online: false, title: 'JY61P 当前样本异常', detail: `${dynamicCopy}；本次 I2C 读取不完整，报警保护已暂停。` };
    }
    return { online: true, title: 'JY61P 在线', detail: `${dynamicCopy}；动态状态有效。` };
  }
  if (!bringup) return { online: false, title: '尚未收到 JY61P 状态帧', detail: '等待 BRINGUP 帧；连接后约每秒更新一次。' };
  if (bringup.jy) return { online: true, title: 'JY61P 已响应', detail: `通信正常（返回码 ${bringup.jyRet}）。` };
  if (bringup.jyRet === 1) return { online: false, title: 'JY61P 未响应', detail: '当前固件正在 I2C1（PB6/PB7）查找地址 0x50；若模块接的是 TX/RX 串口，必须修改 STM32 固件。' };
  return { online: false, title: 'JY61P 读取失败', detail: `设备响应后寄存器读取失败（返回码 ${bringup.jyRet}）。` };
}
Page({
  data: { tone: 'idle', label: '未连接', statusText: '等待操作', deviceName: '—', service: '—', notify: '未开启', raw: '尚无原始数据', logs: [], care: { hr: 0, spo2: 0, fall: false, sos: false }, calibration: { flexZero: null, flexFull: null, imuZero: null }, fingers: [], imu: { roll: '—', pitch: '—', yaw: '—' }, jy: { online: false, title: '尚未收到 JY61P 状态帧', detail: '等待 BRINGUP 帧；连接后约每秒更新一次。' } },
  onLoad() { this.runtime = getApp().getRuntime(); this.active = true; this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state)); }, onShow() { this.active = true; this.applyState(this.runtime.getState()); }, onHide() { this.active = false; }, onUnload() { if (this.unsubscribe) this.unsubscribe(); },
  applyState(state) { if (!this.active) return; this.setData({ tone: tone(state), label: state.connected ? '已连接' : state.reconnecting ? '重连中' : state.connecting ? '连接中' : state.lastError ? '异常' : '未连接', statusText: state.statusText, deviceName: state.deviceName || '—', service: state.serviceId && state.characteristicId ? `${state.serviceId.slice(0, 8)} / ${state.characteristicId.slice(0, 8)}` : '—', notify: state.notifyEnabled ? 'FFE1 Notify 已开启' : 'Notify 未开启', raw: state.lastRawFrame || '尚无原始数据', logs: state.bleLogs, care: state.care, calibration: state.calibration, fingers: FINGER_LABELS.map((label, index) => ({ label, index, enabled: state.calibration.enabledFingers[index] !== false })), imu: { roll: formatNumber(state.rawPose.roll), pitch: formatNumber(state.rawPose.pitch), yaw: formatNumber(state.rawPose.yaw) }, jy: getJyStatus(state.jyStatus, state.bringup) }); },
  scan() { this.runtime.scan().catch(() => {}); }, disconnect() { this.runtime.disconnect().catch(() => {}); },
  calibrate(event) { const method = event.currentTarget.dataset.method; try { this.runtime[method](); wx.showToast({ title: '校准已保存', icon: 'success' }); } catch (error) { wx.showToast({ title: error.message || '校准失败', icon: 'none' }); } },
  toggleFinger(event) { const index = Number(event.currentTarget.dataset.index); try { this.runtime.toggleFinger(index); } catch (error) { wx.showToast({ title: error.message || '切换失败', icon: 'none' }); } },
  goSimulator() { wx.navigateTo({ url: '/pages/simulator/simulator' }); }
});
