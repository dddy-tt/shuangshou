const ALARM_TYPES = Object.freeze({
  FALL: 1,
  JITTER: 2
});

function isValidJYStatus(status) {
  return status === true || (status && status.jy === 1 && status.jyRet === 0);
}

function normalizeAlarmFrame(frame) {
  if (!frame || frame.type !== 'alarm') return null;
  const boot = Number(frame.boot);
  const id = Number(frame.id);
  const alarmType = Number(frame.alarmType);
  if (![boot, id, alarmType].every(Number.isInteger)) return null;
  if (boot < 0 || boot > 0xFFFFFFFF || id < 0 || id > 0xFFFFFFFF) return null;
  if (![ALARM_TYPES.FALL, ALARM_TYPES.JITTER].includes(alarmType)) return null;
  if (typeof frame.active !== 'boolean') return null;
  return { ...frame, boot, id, alarmType };
}

function createSafetyMonitor(options = {}) {
  let jyValid = isValidJYStatus(options.jyValid);

  function pushAlarm(frame, context = {}) {
    const alarm = normalizeAlarmFrame(frame);
    if (!alarm) return null;
    return {
      ...alarm,
      source: context.source || 'ble',
      origin: context.origin || (context.source === 'ble' ? 'device' : 'cloud'),
      deviceId: String(context.deviceId || '').trim(),
      deviceConfirmed: context.deviceConfirmed === true || context.source === 'ble'
    };
  }

  return {
    // 保留旧入口以避免旧页面调用时报错；姿态角变化不再产生安全报警。
    push() { return null; },
    pushAlarm,
    setJYStatus(status) { jyValid = isValidJYStatus(status); },
    isJYValid() { return jyValid; },
    reset() { /* 报警由固件 ALARM 帧驱动，无姿态角采样状态需要清理。 */ }
  };
}

module.exports = { ALARM_TYPES, createSafetyMonitor, isValidJYStatus, normalizeAlarmFrame };
