const { compareGesture, createStableMatcher } = require('../../services/gesture-matcher');
const { createGestureStore } = require('../../services/gesture-store');
const { createTtsService } = require('../../services/tts');

const LABELS = ['左拇指', '左食指', '左中指', '左无名指', '左小指', '右拇指', '右食指', '右中指', '右无名指', '右小指'];
const STATE_TEXT = { straight: '平直', half: '半弯', full: '全弯' };

function formatPose(pose) {
  return {
    roll: typeof pose.roll === 'number' ? pose.roll.toFixed(0) : '--',
    pitch: typeof pose.pitch === 'number' ? pose.pitch.toFixed(0) : '--',
    yaw: typeof pose.yaw === 'number' ? pose.yaw.toFixed(0) : '--'
  };
}
function alarmControls(state, event) {
  const ack = state.alarmAckStatus || { key: '', stage: '' };
  const acknowledged = Boolean(event && event.acknowledgedBy && event.acknowledgedBy.length);
  const sameRequest = Boolean(event && ack.key === event.eventKey);
  const waiting = sameRequest && (ack.stage === 'sending' || ack.stage === 'waiting');
  const failed = sameRequest && ack.stage === 'failed';
  const deviceReady = Boolean(event && state.connected && state.deviceId === event.deviceId && state.guardian.role !== 'guardian');
  return {
    acknowledgeDisabled: acknowledged,
    resolveDisabled: !deviceReady || waiting,
    resolveText: waiting ? (ack.stage === 'sending' ? '正在发送…' : '等待设备回执…') : failed ? '写入失败，点击重试' : deviceReady ? '请求设备解除' : '需连接当前设备',
    resolveStatus: waiting ? '等待设备返回匹配的 ACTIVE=0。' : failed ? '上次请求失败，可以重试。' : ''
  };
}

Page({
  data: {
    connected: false,
    fingers: [],
    enabledFingers: [],
    statusText: '蓝牙未连接',
    target: null,
    targetStates: [],
    countdown: 0,
    training: false,
    resultText: '从你的手势库中随机抽取一个动作',
    feedback: '',
    speechText: '',
    speechStatus: '开始训练后自动播报反馈',
    differences: [],
    stats: { total: 0, correct: 0, streak: 0, accuracy: '0%' },
    pose: { roll: '--', pitch: '--', yaw: '--' },
    safetyAlert: null,
    buzzerStatus: '',
    canResolveAlarm: false,
    guardianRole: 'unselected',
    acknowledgeDisabled: false,
    resolveDisabled: true,
    resolveText: '需连接当前设备',
    resolveStatus: ''
  },

  onLoad() {
    this.runtime = getApp().getRuntime();
    this.store = createGestureStore();
    this.tts = createTtsService();
    this.speechSession = 0;
    this.lastSpeech = '';
    this.lastSpeechAt = 0;
    this.stableMatcher = createStableMatcher({ holdMs: 300 });
    this.active = false;
    this.stats = { total: 0, correct: 0, streak: 0 };
    this.unsubscribe = this.runtime.subscribe((state) => this.applyState(state));
  },

  onShow() {
    this.active = true;
    this.runtime.setMode(this.runtime.modes.rehabilitation);
    this.applyState(this.runtime.getState());
  },

  onHide() {
    this.active = false;
    this.cancelSpeech();
    this.setData({ training: false, countdown: 0 });
    this.clearCountdown();
    this.stableMatcher.reset();
  },

  onUnload() {
    this.onHide();
    if (this.unsubscribe) this.unsubscribe();
  },

  applyState(state) {
    if (!this.active) return;
    const controls = alarmControls(state, state.safetyAlert);
    this.setData({
      connected: state.connected,
      fingers: state.flex,
      enabledFingers: state.calibration.enabledFingers,
      statusText: state.statusText,
      pose: formatPose(state.pose),
      safetyAlert: state.safetyAlert,
      buzzerStatus: state.buzzerStatus,
      guardianRole: state.guardian.role,
      canResolveAlarm: Boolean(state.safetyAlert
        && state.connected
        && state.deviceId === state.safetyAlert.deviceId
        && state.guardian.role !== 'guardian'),
      ...controls
    });
    if (state.mode === this.runtime.modes.rehabilitation && this.data.training && this.data.countdown === 0) {
      this.evaluateCurrent(state);
    }
  },

  chooseTarget() {
    const gestures = this.store.list().filter((item) => item.enabled && !item.needsResample);
    if (!gestures.length) {
      wx.showToast({ title: '请先录入或重新采样有效手势', icon: 'none' });
      return null;
    }
    return gestures[Math.floor(Math.random() * gestures.length)];
  },

  startTraining() {
    const target = this.chooseTarget();
    if (!target) return;
    this.cancelSpeech();
    this.lastSpeech = '';
    this.lastSpeechAt = 0;
    this.setData({ speechText: '', speechStatus: '开始训练后自动播报反馈' });
    this.clearCountdown();
    this.stableMatcher.reset();
    this.setData({ target, targetStates: target.states, countdown: 3, training: false, resultText: '准备开始', feedback: '', differences: [] });
    this.countdownTimer = setInterval(() => {
      const next = this.data.countdown - 1;
      if (next <= 0) {
        this.clearCountdown();
        this.setData({ countdown: 0, training: true, resultText: '请保持目标动作', feedback: '', differences: [] });
        return;
      }
      this.setData({ countdown: next });
    }, 1000);
  },

  clearCountdown() {
    if (this.countdownTimer) clearInterval(this.countdownTimer);
    this.countdownTimer = null;
  },

  evaluateCurrent(state) {
    const enabledFingers = state.calibration.enabledFingers;
    if (!this.data.target || !enabledFingers.some(Boolean) || !Array.isArray(state.fingerStates) || state.fingerStates.some((item, index) => enabledFingers[index] !== false && !item)) {
      this.stableMatcher.update(null);
      return;
    }
    const current = { fingers: state.flex, states: state.fingerStates, pose: state.pose };
    const result = compareGesture(this.data.target, current, { enabledFingers });
    const key = JSON.stringify({ states: current.states, matched: result.matched });
    const stable = this.stableMatcher.update(key, Date.now());
    if (!stable) return;
    this.stats.total += 1;
    if (result.matched) {
      this.stats.correct += 1;
      this.stats.streak += 1;
      this.setData({
        resultText: '动作正确',
        feedback: this.encouragement(),
        differences: [],
        stats: this.displayStats()
      });
      this.announceFeedback('动作正确，继续保持稳定。');
    } else {
      this.stats.streak = 0;
      const differences = result.differences.filter((item) => item.index >= 0).map((item) => ({
        label: LABELS[item.index],
        text: `${LABELS[item.index]}应当${STATE_TEXT[item.expected] || '保持目标状态'}，当前为${STATE_TEXT[item.actual] || '未知状态'}。`
      }));
      this.setData({
        resultText: '动作需要调整',
        feedback: '请按下面提示逐项调整，不需要一次完成所有变化。',
        differences,
        stats: this.displayStats()
      });
      this.announceFeedback(differences.length ? differences[0].text : '手指动作已到位，请调整手腕姿态，与目标保持一致。');
    }
  },

  cancelSpeech() {
    this.speechSession += 1;
    if (this.speechTimer) clearTimeout(this.speechTimer);
    this.speechTimer = null;
    if (this.tts) this.tts.stop();
  },

  announceFeedback(text) {
    this.setData({ speechText: text });
    if (this.speechTimer) clearTimeout(this.speechTimer);
    if (text === this.lastSpeech) return;
    const wait = Math.max(0, 5000 - (Date.now() - this.lastSpeechAt));
    if (wait) this.speechTimer = setTimeout(() => this.playFeedback(), wait);
    else this.playFeedback();
  },

  playFeedback() {
    if (!this.active || !this.data.speechText) return;
    if (this.speechTimer) clearTimeout(this.speechTimer);
    this.speechTimer = null;
    const session = this.speechSession;
    this.lastSpeech = this.data.speechText;
    this.lastSpeechAt = Date.now();
    this.setData({ speechStatus: '正在合成语音…' });
    this.tts.speak(this.data.speechText).then((result) => {
      if (!this.active || session !== this.speechSession) return;
      const status = result.ok ? '已开始播报反馈' : result.reason === 'credentials-missing' ? '请配置百度语音密钥' : result.reason === 'deduplicated' ? '已忽略重复播报' : '播报失败，请点击重播重试';
      this.setData({ speechStatus: status });
    }).catch(() => {
      if (this.active && session === this.speechSession) this.setData({ speechStatus: '播报失败，请点击重播重试' });
    });
  },

  encouragement() {
    const messages = ['动作正确，完成得很好。', '做对了，继续保持稳定。', '很好，这次动作完成准确。'];
    return messages[this.stats.correct % messages.length];
  },

  displayStats() {
    return {
      total: this.stats.total,
      correct: this.stats.correct,
      streak: this.stats.streak,
      accuracy: this.stats.total ? `${Math.round(this.stats.correct / this.stats.total * 100)}%` : '0%'
    };
  },

  stopTraining() {
    this.cancelSpeech();
    this.setData({ speechStatus: '自动播报已暂停' });
    this.clearCountdown();
    this.stableMatcher.reset();
    this.setData({ training: false, countdown: 0, resultText: '训练已暂停' });
  },

  handleAcknowledgeSafety() {
    if (!this.data.acknowledgeDisabled) this.runtime.acknowledgeSafety();
  },

  handleRequestResolve() {
    if (!this.data.safetyAlert || this.data.resolveDisabled) return;
    this.runtime.requestAlarmResolve(this.data.safetyAlert.eventKey).then((result) => {
      if (!result.accepted) wx.showToast({ title: result.reason === 'already-pending' ? '正在等待这次设备回执' : result.reason === 'remote-resolve-forbidden' ? '监护者不能远程解除' : '解除请求失败，可重试', icon: 'none' });
      else wx.showToast({ title: '请求已发送，等待设备返回 ACTIVE=0', icon: 'none' });
    }).catch((error) => wx.showToast({ title: error.message || '解除请求失败', icon: 'none' }));
  },

  goAlarms() {
    wx.navigateTo({ url: '/pages/alarm/alarm' });
  }
});
