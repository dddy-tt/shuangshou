const assert = require('assert');
const {
  classifyFinger,
  classifyFingers,
  compareGesture,
  createStableMatcher
} = require('../services/gesture-matcher');

assert.strictEqual(classifyFinger(0), 'straight');
assert.strictEqual(classifyFinger(29.99), 'straight');
assert.strictEqual(classifyFinger(30), 'half');
assert.strictEqual(classifyFinger(69.99), 'half');
assert.strictEqual(classifyFinger(70), 'full');
assert.deepStrictEqual(
  classifyFingers([0, 30, 70]),
  ['straight', 'half', 'full']
);

const target = {
  fingers: [0, 30, 70],
  states: ['straight', 'half', 'full'],
  pose: null,
  poseTolerance: 8,
  matchPose: false
};

assert.deepStrictEqual(
  compareGesture(target, { fingers: [5, 35, 75], pose: null }),
  { matched: true, differences: [] }
);

assert.deepStrictEqual(
  compareGesture(target, { fingers: [5, 35, 5], pose: null }, { enabledFingers: [true, true, false] }),
  { matched: true, differences: [] }
);

assert.strictEqual(
  compareGesture(target, { fingers: [5, 35, 75], pose: null }, { enabledFingers: [false, false, false] }).matched,
  false
);

const matcher = createStableMatcher({ holdMs: 300 });
assert.strictEqual(matcher.update('hello', 0), null);
assert.strictEqual(matcher.update('hello', 299), null);
assert.deepStrictEqual(matcher.update('hello', 300), { key: 'hello', heldMs: 300 });
assert.strictEqual(matcher.update('hello', 301), null);
assert.strictEqual(matcher.update('world', 302), null);

console.log('gesture matcher tests passed');
