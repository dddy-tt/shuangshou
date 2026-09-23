const assert = require('assert');

let definition;
global.Page = (config) => { definition = config; };

const runtimeState = {
  connected: true,
  connecting: false,
  reconnecting: false,
  lastError: '',
  flex: [12.345, 20, 30, 40, 50, 60, 70, 80, 90, 100],
  fingerStates: ['straight', 'straight', 'half', 'half', 'full', 'full', 'full', 'full', 'full', 'full'],
  pose: { roll: 1.234, pitch: -2.345, yaw: 63.846 },
  care: { hr: 0, spo2: 0, fall: false, sos: false },
  calibration: { enabledFingers: Array(10).fill(true) },
  lastFrameAt: 1
};

const runtime = {
  modes: { translation: 'translation' },
  subscribe() { return () => {}; },
  getState() { return runtimeState; },
  setMode() {},
  setRecognition() {}
};
global.getApp = () => ({ getRuntime: () => runtime });
require('../pages/translation/translation');

const calls = [];
const page = {
  ...definition,
  data: JSON.parse(JSON.stringify(definition.data)),
  setData(patch) {
    calls.push(patch);
    Object.assign(this.data, patch);
  }
};

page.onLoad();
page.onShow();
assert.strictEqual(page.data.left[0].value, 12.3);
assert.strictEqual(page.data.translation, '等待手势');
assert.strictEqual(page.data.guardianRole, 'unselected');
assert.strictEqual(page.data.resolveDisabled, true);

calls.length = 0;
page.applyState({ ...runtimeState, pose: { roll: 8.765, pitch: -4.321, yaw: 64.123 }, lastFrameAt: 2 });
assert.ok(calls.every((patch) => !Object.prototype.hasOwnProperty.call(patch, 'left') && !Object.prototype.hasOwnProperty.call(patch, 'right')));

page.onHide();
console.log('translation page tests passed');
