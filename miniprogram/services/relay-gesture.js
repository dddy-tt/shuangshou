const { compareGesture } = require('./gesture-matcher');

const DEFAULT_DEVICE_ID = 'light';

function normalizeRelayAction(action) {
  const value = String(action || '').trim().toUpperCase();
  if (value === 'LIGHT_ON') return 'ON';
  if (value === 'LIGHT_OFF') return 'OFF';
  return ['ON', 'OFF'].includes(value) ? value : '';
}

function normalizeTargetId(value) {
  const id = String(value || '').trim();
  if (!id) return '';
  return id.toLowerCase() === DEFAULT_DEVICE_ID ? DEFAULT_DEVICE_ID : id.toUpperCase();
}

function getBinding(bindings, gestureId) {
  if (!bindings || !gestureId) return null;
  if (Array.isArray(bindings)) {
    return bindings.find((item) => item && String(item.gestureId) === String(gestureId)) || null;
  }
  if (typeof bindings.get === 'function') return bindings.get(gestureId) || null;
  return bindings[gestureId] || null;
}

function resolveGestureTarget(gesture = {}, options = {}) {
  const binding = getBinding(options.bindings, gesture.id);
  if (binding && (binding.disabled === true || binding.mode === 'disabled')) {
    return {
      valid: false,
      reason: '该手势原绑定设备已删除或不可用，请显式重新绑定。',
      gestureId: gesture.id || null,
      binding: binding || null
    };
  }
  const explicitIds = Array.isArray(gesture.deviceIds) ? gesture.deviceIds.filter(Boolean) : null;
  if (explicitIds && explicitIds.length !== 1) {
    return { valid: false, reason: '一个手势只能绑定一个设备。', gestureId: gesture.id || null };
  }

  const explicitId = normalizeTargetId(explicitIds ? explicitIds[0] : (gesture.deviceId || gesture.targetDeviceId));
  const followsSelected = Boolean(binding
    && (binding.mode === 'follow-selected' || binding.followSelected === true));
  const bindingId = followsSelected
    ? ''
    : normalizeTargetId(binding && (binding.deviceId || binding.targetDeviceId || binding.id));
  if (followsSelected && explicitId) {
    return { valid: false, reason: '跟随当前选择的手势不能同时声明固定设备。', gestureId: gesture.id || null };
  }
  if (explicitId && bindingId && explicitId.toLowerCase() !== bindingId.toLowerCase()) {
    return { valid: false, reason: '一个手势不能同时绑定多个设备。', gestureId: gesture.id || null };
  }
  // 没有绑定时保持 legacy light 兼容；只有明确保存了 follow-selected
  // 模式，才允许当前选择设备参与解析，避免删除设备后静默迁移。
  const selectedId = normalizeTargetId(options.selectedDeviceId);
  const deviceId = explicitId || bindingId || (followsSelected ? selectedId : DEFAULT_DEVICE_ID);
  if (!deviceId) return { valid: false, reason: '控制手势没有目标设备。', gestureId: gesture.id || null };

  const gestureAction = normalizeRelayAction(gesture.action || gesture.text);
  const bindingAction = normalizeRelayAction(binding && binding.action);
  if (gestureAction && bindingAction && gestureAction !== bindingAction) {
    return { valid: false, reason: '手势动作与设备绑定动作不一致。', gestureId: gesture.id || null };
  }
  const action = gestureAction || bindingAction;
  if (!action) return { valid: false, reason: '控制手势动作只能是 ON 或 OFF。', gestureId: gesture.id || null };

  return {
    valid: true,
    deviceId,
    action,
    gestureId: gesture.id || null,
    binding: binding || null
  };
}

function getPendingDeviceIds(options = {}) {
  const pendingDeviceIds = options.pendingDeviceIds instanceof Set
    ? [...options.pendingDeviceIds]
    : Array.isArray(options.pendingDeviceIds) ? options.pendingDeviceIds : [];
  const ids = new Set(pendingDeviceIds.map((id) => String(id).toLowerCase()));
  const statuses = options.statuses || options.deviceStatuses || {};
  Object.keys(statuses).forEach((id) => {
    if (statuses[id] && statuses[id].pending) ids.add(String(id).toLowerCase());
  });
  return ids;
}

function collectRelayMatches(gestures, state, options = {}) {
  const enabledFingers = Array.isArray(options.enabledFingers)
    ? options.enabledFingers
    : state && state.calibration && state.calibration.enabledFingers;
  const current = {
    fingers: state && state.flex,
    states: state && state.fingerStates,
    pose: state && state.pose
  };
  const matches = [];
  (Array.isArray(gestures) ? gestures : []).forEach((gesture) => {
    if (!gesture || gesture.enabled === false || gesture.category !== 'control') return;
    const binding = getBinding(options.bindings, gesture.id);
    const declaredAction = normalizeRelayAction(gesture.action || gesture.text)
      || normalizeRelayAction(binding && binding.action);
    if (!declaredAction) return;
    if (!compareGesture(gesture, current, { enabledFingers }).matched) return;
    const target = resolveGestureTarget(gesture, options);
    if (!target.valid) {
      matches.push({ gesture, invalid: true, reason: target.reason });
      return;
    }
    matches.push({ ...target, gesture });
  });
  return matches;
}

function createRelayGestureGate() {
  let key = null;
  let since = 0;
  let latched = false;

  function reset() {
    key = null;
    since = 0;
    latched = false;
  }

  function update(gestures, state = {}, nowOrOptions = Date.now()) {
    const options = typeof nowOrOptions === 'number'
      ? { now: nowOrOptions }
      : (nowOrOptions || {});
    const routed = typeof nowOrOptions !== 'number'
      && Object.keys(options).some((name) => name !== 'now');
    const now = Number.isFinite(Number(options.now)) ? Number(options.now) : Date.now();
    const enabled = Array.isArray(options.enabledFingers)
      ? options.enabledFingers
      : state.calibration && state.calibration.enabledFingers;
    const flex = Array.isArray(state.flex) ? state.flex : [];
    const requireLastFlexAt = Boolean(options.requireLastFlexAt);
    const lastFlexAt = Number(state.lastFlexAt);
    const lastFrameAt = Number(state.lastFrameAt);
    const hasLastFlexAt = Number.isFinite(lastFlexAt) && lastFlexAt > 0;
    const flexTimestamp = hasLastFlexAt ? lastFlexAt : lastFrameAt;
    if (requireLastFlexAt && !hasLastFlexAt) {
      reset();
      return { message: '等待新的 FLEX 数据（需要 lastFlexAt）' };
    }
    const validData = Boolean(
      state.connected
      && Number.isFinite(flexTimestamp)
      && flexTimestamp > 0
      && now - flexTimestamp >= 0
      && now - flexTimestamp <= 1500
      && Array.isArray(enabled)
      && enabled.some(Boolean)
      && enabled.every((on, index) => !on || Number.isFinite(flex[index]))
    );
    if (!validData) {
      reset();
      return { message: '等待有效的手套数据' };
    }

    const matches = collectRelayMatches(gestures, state, {
      ...options,
      enabledFingers: enabled
    });
    const invalidMatch = matches.find((item) => item.invalid);
    if (invalidMatch) {
      reset();
      return { message: invalidMatch.reason || '控制手势绑定无效，请重新绑定' };
    }

    const targetKeys = [...new Set(matches.map((item) => `${item.deviceId}\u0000${item.action}`))];
    if (targetKeys.length !== 1) {
      key = null;
      if (!matches.length) latched = false;
      if (!matches.length) return { message: '等待控制手势' };
      const targetIds = [...new Set(matches.map((item) => item.deviceId))];
      if (targetIds.length > 1) return { message: '一个手势只能控制一个设备，请为各设备分别绑定手势' };
      return { message: '开启和关闭手势冲突，请重新录入' };
    }

    const target = matches[0];
    if (getPendingDeviceIds(options).has(String(target.deviceId).toLowerCase())) {
      return { message: '等待设备回报，请松开手势' };
    }
    if (latched) return { message: '请松开手势后再次操作' };

    const candidateKey = `${target.deviceId}\u0000${target.action}`;
    if (key !== candidateKey) {
      key = candidateKey;
      since = now;
    }
    if (now - since < 800) {
      return { message: `已识别 ${target.action}，请保持 0.8 秒` };
    }
    latched = true;
    const triggered = { action: target.action, message: `触发 ${target.action}，请松开手势` };
    if (routed) {
      triggered.deviceId = target.deviceId;
      triggered.gestureId = target.gestureId;
    }
    return triggered;
  }

  return { reset, update };
}

module.exports = {
  DEFAULT_DEVICE_ID,
  normalizeRelayAction,
  resolveGestureTarget,
  collectRelayMatches,
  createRelayGestureGate
};
