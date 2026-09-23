const { ROLES } = require('../../services/guardian-binding');

function timeText(timestamp) {
  return timestamp ? new Date(timestamp).toLocaleString('zh-CN', { hour12: false }) : '—';
}

Page({
  data: { active: [], history: [], role: ROLES.UNSELECTED, alarmAudioStatus: '本地报警音待触发', guardianStatus: '', alarmAckStatus: { stage: 'idle', key: '' } },

  onLoad() {
    this.runtime = getApp().getRuntime();
    this.activePage = true;
    this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state));
  },

  onShow() {
    this.activePage = true;
    this.applyState(this.runtime.getState());
    const state = this.runtime.getState();
    if (state.guardian.role === ROLES.GUARDIAN && state.guardian.binding) this.runtime.startGuardianMonitor();
  },

  onHide() { this.activePage = false; },

  onUnload() { if (this.unsubscribe) this.unsubscribe(); },

  applyState(state) {
    if (!this.activePage) return;
    const alarm = state.alarm || { active: [], history: [] };
    const ackStatus = state.alarmAckStatus || { stage: 'idle', key: '' };
    const role = state.guardian.role;
    const toViewEvent = (item) => {
      const acknowledged = item.acknowledgedBy && item.acknowledgedBy.length > 0;
      const sameRequest = ackStatus.key === item.eventKey;
      const waiting = sameRequest && (ackStatus.stage === 'sending' || ackStatus.stage === 'waiting');
      const failed = sameRequest && ackStatus.stage === 'failed';
      const deviceReady = Boolean(state.connected && state.deviceId === item.deviceId && role !== ROLES.GUARDIAN);
      return {
        ...item,
        time: timeText(item.timestamp || item.firstSeenAt),
        acknowledged,
        acknowledgeDisabled: acknowledged,
        sourceText: (item.sources || []).join('、') || '—',
        resolveDisabled: !deviceReady || waiting,
        resolveText: waiting ? (ackStatus.stage === 'sending' ? '正在发送…' : '等待设备回执…') : failed ? '写入失败，点击重试' : deviceReady ? '请求设备解除' : '需连接当前设备',
        resolveStatusText: waiting ? '手机已发送请求，设备返回匹配 ACTIVE=0 前不会解除。' : failed ? `上次请求失败：${ackStatus.detail || 'BLE 写入失败'}。可以重试。` : ''
      };
    };
    this.setData({
      active: alarm.active.map(toViewEvent),
      history: alarm.history.map((item) => ({ ...item, time: timeText(item.timestamp || item.firstSeenAt), acknowledged: item.acknowledgedBy && item.acknowledgedBy.length > 0, sourceText: (item.sources || []).join('、') || '—' })),
      role,
      alarmAudioStatus: state.alarmAudioStatus,
      guardianStatus: state.guardian.statusText,
      alarmAckStatus: ackStatus
    });
  },

  acknowledge(event) {
    const key = event.currentTarget.dataset.key;
    const actor = this.data.role === ROLES.GUARDIAN ? 'guardian' : 'local';
    this.runtime.acknowledgeAlarm(key, actor).then((result) => {
      if (result.accepted) wx.showToast({ title: '已确认，活动状态未清除', icon: 'none' });
    });
  },

  requestResolve(event) {
    const key = event.currentTarget.dataset.key;
    this.runtime.requestAlarmResolve(key).then((result) => {
      if (!result.accepted) {
        const title = result.reason === 'remote-resolve-forbidden'
          ? '监护者不能远程解除'
          : result.reason === 'already-pending'
            ? '正在等待这次设备回执'
            : ['write-failed', 'ack-failed', 'active-zero-timeout'].includes(result.reason)
              ? '写入或设备回执失败，可以重试'
              : result.reason === 'stale-device' ? '当前设备或启动会话已变化' : '事件已解除或不存在';
        wx.showToast({ title, icon: 'none' });
      } else wx.showToast({ title: '请求已发送，等待设备返回 ACTIVE=0', icon: 'none' });
    }).catch((error) => wx.showToast({ title: error.message || '解除请求发送失败', icon: 'none' }));
  },

  openGuardian() { wx.navigateTo({ url: '/pages/guardian/guardian' }); }
});
