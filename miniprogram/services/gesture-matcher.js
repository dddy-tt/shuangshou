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
  const targetStates = Array.isArray(target && target.states)
    ? target.states
    : classifyFingers(target && target.fingers);
  const currentStates = Array.isArray(current && current.states)
    ? current.states
    : classifyFingers(current && current.fingers);
  const differences = [];
  const length = Math.max(targetStates.length, currentStates.length);
  const enabledFingers = Array.isArray(options.enabledFingers) ? options.enabledFingers : null;
  let enabledCount = 0;

  for (let index = 0; index < length; index += 1) {
    if (enabledFingers && enabledFingers[index] === false) continue;
    enabledCount += 1;
    if (targetStates[index] !== currentStates[index]) {
      differences.push({
        index,
        expected: targetStates[index] || null,
        actual: currentStates[index] || null
      });
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
