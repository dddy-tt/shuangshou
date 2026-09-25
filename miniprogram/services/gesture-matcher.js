const FINGER_STATES = Object.freeze({
  straight: 'straight',
  half: 'half',
  full: 'full'
});

function classifyFinger(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) {
    return null;
  }
  if (number < 30) {
    return FINGER_STATES.straight;
  }
  if (number < 70) {
    return FINGER_STATES.half;
  }
  return FINGER_STATES.full;
}

function classifyFingers(values) {
  return (Array.isArray(values) ? values : []).map(classifyFinger);
}

function hasFingerMask(mask) {
  return Array.isArray(mask)
    && mask.length === 10
    && mask.every((enabled) => typeof enabled === 'boolean');
}

function poseMatches(targetPose, currentPose, tolerance) {
  if (!targetPose || !currentPose) {
    return false;
  }
  const limit = Number.isFinite(Number(tolerance)) ? Number(tolerance) : 8;
  return ['roll', 'pitch', 'yaw'].every((axis) => {
    const target = Number(targetPose[axis]);
    const current = Number(currentPose[axis]);
    return Number.isFinite(target) && Number.isFinite(current)
      && Math.abs(target - current) <= limit;
  });
}

function compareGesture(target, current, options = {}) {
  const targetStates = Array.isArray(target && target.states) && target.states.length === 10
    ? target.states
    : classifyFingers(target && target.fingers);
  const currentStates = Array.isArray(current && current.states) && current.states.length === 10
    ? current.states
    : classifyFingers(current && current.fingers);
  const differences = [];
  const sampledFingers = target && target.enabledFingers;
  const enabledFingers = options.enabledFingers;
  const hasSampleMask = hasFingerMask(sampledFingers);
  const hasCurrentMask = hasFingerMask(enabledFingers);
  let enabledCount = 0;

  if (!hasSampleMask) {
    differences.push({ index: -3, expected: 'sampled-finger-mask', actual: 'missing-or-invalid' });
  }
  if (!hasCurrentMask) {
    differences.push({ index: -4, expected: 'current-finger-mask', actual: 'missing-or-invalid' });
  }

  if (hasSampleMask && hasCurrentMask) {
    for (let index = 0; index < 10; index += 1) {
      if (sampledFingers[index] !== true || enabledFingers[index] !== true) continue;
      enabledCount += 1;
      if (!targetStates[index] || !currentStates[index] || targetStates[index] !== currentStates[index]) {
        differences.push({
          index,
          expected: targetStates[index] || null,
          actual: currentStates[index] || null
        });
      }
    }
  }

  if (enabledCount === 0) {
    differences.push({ index: -2, expected: 'enabled-finger', actual: 'none' });
  }

  const poseRequired = Boolean(target && target.matchPose);
  const poseMatched = !poseRequired || poseMatches(
    target.pose,
    current && current.pose,
    target.poseTolerance
  );
  if (poseRequired && !poseMatched) {
    differences.push({ index: -1, expected: 'pose', actual: 'pose' });
  }

  return { matched: differences.length === 0, differences };
}

function createStableMatcher(options = {}) {
  const holdMs = Number.isFinite(Number(options.holdMs)) ? Number(options.holdMs) : 300;
  let candidateKey = null;
  let startedAt = 0;
  let emitted = false;

  return {
    update(key, now = Date.now()) {
      if (!key) {
        candidateKey = null;
        emitted = false;
        return null;
      }
      if (candidateKey !== key) {
        candidateKey = key;
        startedAt = now;
        emitted = false;
        return null;
      }
      const heldMs = Math.max(0, now - startedAt);
      if (!emitted && heldMs >= holdMs) {
        emitted = true;
        return { key, heldMs };
      }
      return null;
    },
    reset() {
      candidateKey = null;
      startedAt = 0;
      emitted = false;
    },
    getCandidate() {
      return candidateKey;
    }
  };
}

module.exports = {
  FINGER_STATES,
  classifyFinger,
  classifyFingers,
  compareGesture,
  createStableMatcher,
  poseMatches
};
