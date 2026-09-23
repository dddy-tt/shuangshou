const DISPLAY_DIGITS = 1;

function isMetric(value) {
  return value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value));
}

function roundForUi(value, digits = DISPLAY_DIGITS) {
  if (!isMetric(value)) return 0;
  return Number(Number(value).toFixed(digits));
}

function formatMetric(value, fallback = '--') {
  return isMetric(value) ? Number(value).toFixed(DISPLAY_DIGITS) : fallback;
}

function nextPose(current, target, factor = 0.34) {
  const next = {};
  ['roll', 'pitch', 'yaw'].forEach((axis) => {
    const targetValue = target && target[axis];
    const currentValue = current && current[axis];
    if (!isMetric(targetValue)) {
      next[axis] = isMetric(currentValue) ? Number(currentValue) : null;
    } else if (!isMetric(currentValue)) {
      next[axis] = Number(targetValue);
    } else {
      next[axis] = Number(currentValue) + (Number(targetValue) - Number(currentValue)) * factor;
    }
  });
  return next;
}

module.exports = { DISPLAY_DIGITS, formatMetric, isMetric, nextPose, roundForUi };
