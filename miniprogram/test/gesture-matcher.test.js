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
  enabledFingers: [true, true, true, false, false, false, false, false, false, false],
  pose: null,
  poseTolerance: 8,
  matchPose: false
};

assert.deepStrictEqual(
  compareGesture(target, { fingers: [5, 35, 75], pose: null }, { enabledFingers: Array(10).fill(true) }),
  { matched: true, differences: [] }
);

assert.deepStrictEqual(
  compareGesture(target, { fingers: [5, 35, 5], pose: null }, { enabledFingers: [true, true, false, false, false, false, false, false, false, false] }),
  { matched: true, differences: [] }
);

assert.strictEqual(
  compareGesture(target, { fingers: [5, 35, 75], pose: null }, { enabledFingers: Array(10).fill(false) }).matched,
  false
);

const legacyThumbSample = {
  // 旧模板当时只启用了拇指；无名指和小指留有陈旧的全弯数据。
  fingers: [10, 20, 20, 95, 96, 20, 20, 20, 20, 20],
  states: ['straight', 'straight', 'straight', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight'],
  pose: null,
  matchPose: false
};
const ringAndLittleNowEnabled = [false, false, false, true, true, false, false, false, false, false];
const currentRingAndLittlePose = {
  fingers: [99, 20, 20, 94, 95, 20, 20, 20, 20, 20],
  states: ['full', 'straight', 'straight', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight'],
  pose: null
};
assert.strictEqual(
  compareGesture(legacyThumbSample, currentRingAndLittlePose, { enabledFingers: ringAndLittleNowEnabled }).matched,
  false,
  '没有采样掩码的历史模板不能让陈旧无名指/小指状态造成误命中'
);

const partialSample = {
  ...target,
  states: ['straight', 'half', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight', 'straight'],
  enabledFingers: [true, false, false, false, false, false, false, false, false, false]
};
const currentThumbDisabled = [false, true, false, false, false, false, false, false, false, false];
assert.strictEqual(
  compareGesture(partialSample, currentRingAndLittlePose, { enabledFingers: currentThumbDisabled }).matched,
  false,
  '采样掩码与当前掩码无交集时必须拒绝匹配'
);

const overlappingSample = {
  ...target,
  states: ['straight', 'half', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight', 'straight'],
  enabledFingers: [true, true, false, false, false, false, false, false, false, false]
};
const currentWithOverlap = {
  fingers: [10, 20, 20, 94, 95, 20, 20, 20, 20, 20],
  states: ['straight', 'straight', 'straight', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight'],
  pose: null
};
assert.deepStrictEqual(
  compareGesture(overlappingSample, currentWithOverlap, { enabledFingers: [true, false, false, true, true, false, false, false, false, false] }),
  { matched: true, differences: [] },
  '只比较采样时与当前都启用的拇指，忽略掩码外交叉差异'
);

assert.strictEqual(
  compareGesture(target, { fingers: [5, 35, 75], pose: null }).matched,
  false,
  '没有当前启用掩码时应安全拒绝匹配'
);

const matcher = createStableMatcher({ holdMs: 300 });
assert.strictEqual(matcher.update('hello', 0), null);
assert.strictEqual(matcher.update('hello', 299), null);
assert.deepStrictEqual(matcher.update('hello', 300), { key: 'hello', heldMs: 300 });
assert.strictEqual(matcher.update('hello', 301), null);
assert.strictEqual(matcher.update('world', 302), null);

console.log('gesture matcher tests passed');
