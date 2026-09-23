const assert = require('assert');
const { mapFlex, offsetImu, validFlex, normalizeEnabledFingers } = require('../services/calibration');

const zero = Array(10).fill(80);
const full = Array(10).fill(20);
assert.deepStrictEqual(mapFlex(zero, zero, full), Array(10).fill(0));
assert.deepStrictEqual(mapFlex(full, zero, full), Array(10).fill(100));
assert.strictEqual(validFlex(Array(10).fill(null)), false);
assert.deepStrictEqual(normalizeEnabledFingers(null), Array(10).fill(true));
assert.deepStrictEqual(normalizeEnabledFingers([false, true]), [false, true, true, true, true, true, true, true, true, true]);
assert.deepStrictEqual(offsetImu({ roll: 5, pitch: -2, yaw: 10 }, { roll: 5, pitch: -2, yaw: 10 }), { roll: 0, pitch: 0, yaw: 0 });
console.log('calibration tests passed');
