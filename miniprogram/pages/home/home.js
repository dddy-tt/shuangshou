function timeText(timestamp) {
  return timestamp ? new Date(timestamp).toLocaleTimeString('zh-CN', { hour12: false }) : '尚未收到数据';
}
function toneFor(state) {
  if (state.connected) return 'success';
  if (state.connecting || state.discovering || state.reconnecting) return 'warning';
  return state.lastError ? 'danger' : 'idle';
}
function alarmControls(state, event) {
  const ack = state.alarmAckStatus || { key: '', stage: '' };
  const acknowledgements = event && Array.isArray(event.acknowledgedBy) ? event.acknowledgedBy : [];
  const locallyAcknowledged = acknowledgements.includes('local');
  const remotelyAcknowledged = acknowledgements.some((actor) => actor !== 'local');
  const sameRequest = Boolean(event && ack.key === event.eventKey);
  const waiting = sameRequest && (ack.stage === 'sending' || ack.stage === 'waiting');
  const failed = sameRequest && ack.stage === 'failed';
  const deviceReady = Boolean(event && state.connected && state.deviceId === event.deviceId && state.guardian.role !== 'guardian');
  return {
    acknowledgeAlarmDisabled: locallyAcknowledged,
    acknowledgeAlarmText: locallyAcknowledged ? '本地提示已停止' : '确认并停止本地提示',
    alarmStatusText: locallyAcknowledged
      ? state.connected ? '本机已确认，等待固件 ACTIVE=0。' : '本机已确认；设备未连接，状态待核验。'
      : remotelyAcknowledged ? '监护端已确认；请确认本机提示。' : '请立即确认佩戴者状态。',
    canHideLocalAlarm: Boolean(event && locallyAcknowledged && !state.connected && state.guardian.role !== 'guardian'),
    resolveAlarmDisabled: !deviceReady || waiting,
    resolveAlarmText: waiting ? (ack.stage === 'sending' ? '正在发送…' : '等待设备回执…') : failed ? '写入失败，点击重试' : deviceReady ? '请求设备解除' : '需连接当前设备',
    resolveAlarmStatus: waiting ? '等待设备返回匹配的 ACTIVE=0。' : failed ? '上次请求失败，可以重试。' : ''
  };
}
Page({
  data: { statusTone: 'idle', statusLabel: '未连接', statusText: '请搜索附近的 JDY-23。', devices: [], deviceName: 'JDY-23', lastFrame: '尚未收到数据', connected: false, connecting: false, lastError: '', recentResult: '等待手势', recentCopy: '接收到完整手势数据后，系统会在翻译页进行稳定识别。', activeAlarm: null, alarmAudioStatus: '本地报警音待触发', guardianRole: 'unselected', guardianStatus: '监护服务未配置', acknowledgeAlarmDisabled: false, acknowledgeAlarmText: '确认并停止本地提示', alarmStatusText: '请立即确认佩戴者状态。', canHideLocalAlarm: false, resolveAlarmDisabled: true, resolveAlarmText: '需连接当前设备', resolveAlarmStatus: '' },
  onLoad() { this.runtime = getApp().getRuntime(); this.active = true; this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state)); },
  onShow() { this.active = true; this.applyState(this.runtime.getState()); },
  onHide() { this.active = false; },
  onUnload() { if (this.unsubscribe) this.unsubscribe(); },
  applyState(state) {
    if (!this.active) return;
    const connected = Boolean(state.connected);
    const recognition = state.recognition || {};
    const activeAlarm = state.alarm && state.alarm.active.length ? state.alarm.active[0] : null;
    this.setData({
      statusTone: toneFor(state),
      statusLabel: connected ? '已连接' : state.reconnecting ? '重连中' : state.connecting ? '连接中' : state.discovering ? '搜索中' : state.lastError ? '连接异常' : '未连接',
      statusText: state.statusText,
      connected,
      connecting: Boolean(state.connecting || state.reconnecting),
      devices: state.devices.map((device) => ({ ...device, displayName: device.localName || device.name || 'JDY-23', rssi: typeof device.RSSI === 'number' ? `${device.RSSI} dBm` : '信号未知' })),
      deviceName: state.deviceName || 'JDY-23', lastFrame: timeText(state.lastFrameAt), lastError: state.lastError,
      recentResult: recognition.text || '等待手势', recentCopy: recognition.status || '接收到完整手势数据后，系统会在翻译页进行稳定识别。', activeAlarm, alarmAudioStatus: state.alarmAudioStatus, guardianRole: state.guardian.role, guardianStatus: state.guardian.statusText, ...alarmControls(state, activeAlarm)
    });
  },
  async handleScan() { try { await this.runtime.scan(); } catch (error) { wx.showToast({ title: '无法开始蓝牙搜索', icon: 'none' }); } },
  async handleConnect(event) { const device = this.data.devices[event.currentTarget.dataset.index]; if (!device) return; try { await this.runtime.connect(device); } catch (error) { wx.showToast({ title: '连接失败，请查看设备状态', icon: 'none' }); } },
  handleDisconnect() { this.runtime.disconnect().catch(() => {}); },
  goTranslation() { wx.switchTab({ url: '/pages/translation/translation' }); },
  goTrain() { wx.navigateTo({ url: '/pages/gesture-train/gesture-train' }); },
  goLibrary() { wx.navigateTo({ url: '/pages/gesture-library/gesture-library' }); },
  goSettings() { wx.navigateTo({ url: '/pages/settings/settings' }); },
  goAlarms() { wx.navigateTo({ url: '/pages/alarm/alarm' }); },
  goGuardian() { wx.navigateTo({ url: '/pages/guardian/guardian' }); },
  handleAcknowledgeAlarm() { if (this.data.activeAlarm && !this.data.acknowledgeAlarmDisabled) this.runtime.acknowledgeAlarm(this.data.activeAlarm.eventKey, 'local'); },
  handleResolveAlarm() {
    if (!this.data.activeAlarm || this.data.resolveAlarmDisabled) return;
    this.runtime.requestAlarmResolve(this.data.activeAlarm.eventKey).then((result) => {
      if (!result.accepted) wx.showToast({ title: result.reason === 'already-pending' ? '正在等待这次设备回执' : result.reason === 'remote-resolve-forbidden' ? '监护者不能远程解除' : '解除请求失败，可重试', icon: 'none' });
      else wx.showToast({ title: '请求已发送，等待设备返回 ACTIVE=0', icon: 'none' });
    }).catch((error) => wx.showToast({ title: error.message || '解除请求发送失败', icon: 'none' }));
  },
  handleHideLocalAlarm() {
    if (!this.data.activeAlarm || !this.data.canHideLocalAlarm) return;
    const eventKey = this.data.activeAlarm.eventKey;
    wx.showModal({
      title: '仅隐藏本机旧提示',
      content: '设备当前未连接，无法确认蜂鸣器和固件报警是否已解除。此操作不会向设备发送命令；重新连接后若设备仍报告活动报警，提示会再次出现。',
      confirmText: '仅隐藏本机提示',
      confirmColor: '#a23936',
      success: ({ confirm }) => {
        if (!confirm) return;
        this.runtime.hideLocalAlarm(eventKey).then((result) => {
          if (result.accepted) wx.showToast({ title: '已隐藏本机旧提示', icon: 'none' });
          else wx.showToast({ title: result.reason === 'device-connected' ? '设备已连接，请等待状态核验' : '暂时无法隐藏该提示', icon: 'none' });
        }).catch((error) => wx.showToast({ title: error.message || '操作失败', icon: 'none' }));
      }
    });
  }
});
