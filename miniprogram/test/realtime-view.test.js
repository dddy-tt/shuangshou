const assert = require('assert');
const {
  formatMetric,
  roundForUi,
  nextPose
} = require('../utils/realtime-view');

assert.strictEqual(formatMetric(63.84692), '63.8');
assert.strictEqual(formatMetric(-2.34567), '-2.3');
assert.strictEqual(formatMetric(null), '--');
assert.strictEqual(roundForUi(61.234567), 61.2);

const first = nextPose(
  { roll: 0, pitch: 0, yaw: 0 },
  { roll: 10, pitch: -10, yaw: 20 },
  0.3
);
assert.deepStrictEqual(first, { roll: 3, pitch: -3, yaw: 6 });

const initial = nextPose(
  { roll: null, pitch: null, yaw: null },
  { roll: 1.234, pitch: -2.345, yaw: 63.846 },
  0.3
);
assert.deepStrictEqual(initial, { roll: 1.234, pitch: -2.345, yaw: 63.846 });

console.log('realtime view tests passed');
