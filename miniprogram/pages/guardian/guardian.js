const { ROLES } = require('../../services/guardian-binding');

function timeText(timestamp) {
  return timestamp ? new Date(timestamp).toLocaleString('zh-CN', { hour12: false }) : '—';
}

Page({
  data: {
    configured: false,
    role: ROLES.UNSELECTED,
    binding: null,
    invite: null,
    inviteText: '',
    inviteExpiry: '—',
    inviteCode: '',
    statusText: '监护服务未配置',
    lastError: '',
    online: false,
    lastSeenText: '—',
    activeAlarm: null,
    alarmAudioStatus: '本地报警音待触发',
    outboxText: '本地上报队列为空'
  },

  onLoad() {
    this.runtime = getApp().getRuntime();
    this.active = true;
    this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state));
  },

  onShow() {
    this.active = true;
    this.applyState(this.runtime.getState());
    this.refreshBinding();
  },

  onHide() { this.active = false; },

  onUnload() {
    if (this.unsubscribe) this.unsubscribe();
  },

  applyState(state) {
    if (!this.active) return;
    const guardian = state.guardian || {};
    const activeAlarm = state.alarm && state.alarm.active.length ? state.alarm.active[0] : null;
    const outbox = guardian.outbox || {};
    this.setData({
      configured: Boolean(guardian.configured),
      role: guardian.role || ROLES.UNSELECTED,
      binding: guardian.binding || null,
      invite: guardian.invite || null,
      inviteText: guardian.invite ? guardian.invite.code : '',
      inviteExpiry: guardian.invite ? timeText(guardian.invite.expiresAt) : '—',
      statusText: guardian.statusText || '—',
      lastError: guardian.lastError || '',
      online: Boolean(guardian.online),
      lastSeenText: timeText(guardian.lastSeenAt),
      activeAlarm,
      alarmAudioStatus: state.alarmAudioStatus,
      outboxText: outbox.pending ? `待上报 ${outbox.pending} 条；回到前台后按顺序重试。` : '本地上报队列为空'
    });
  },

  selectRole(event) {
    try {
      const role = event.currentTarget.dataset.role;
      this.runtime.setGuardianRole(role);
      if (role === ROLES.GUARDIAN) this.refreshBinding();
      wx.showToast({ title: role === ROLES.WEARER ? '已选择佩戴者' : '已选择监护者', icon: 'success' });
    } catch (error) {
      wx.showToast({ title: error.message || '角色选择失败', icon: 'none' });
    }
  },

  refreshBinding() {
    this.runtime.refreshGuardian().then((state) => {
      if (state.role === ROLES.GUARDIAN && state.binding) this.runtime.startGuardianMonitor();
    }).catch(() => {});
  },

  createInvite() {
    this.runtime.createGuardianInvite().then(() => {
      wx.showToast({ title: '邀请码已生成', icon: 'success' });
    }).catch((error) => wx.showToast({ title: error.message || '生成邀请码失败', icon: 'none' }));
  },

  onInviteInput(event) { this.setData({ inviteCode: String(event.detail.value || '').toUpperCase() }); },

  acceptInvite() {
    const code = String(this.data.inviteCode || '').trim();
    if (!code) { wx.showToast({ title: '请输入邀请码', icon: 'none' }); return; }
    this.runtime.acceptGuardianInvite(code).then(() => {
      wx.showToast({ title: '绑定成功', icon: 'success' });
      this.runtime.startGuardianMonitor();
    }).catch((error) => wx.showToast({ title: error.message || '绑定失败', icon: 'none' }));
  },

  acknowledgeAlarm() {
    if (!this.data.activeAlarm) return;
    this.runtime.acknowledgeAlarm(this.data.activeAlarm.eventKey, 'guardian').then(() => {
      wx.showToast({ title: '已确认，设备仍需自行解除', icon: 'none' });
    });
  },

  openAlarms() { wx.navigateTo({ url: '/pages/alarm/alarm' }); }
});
