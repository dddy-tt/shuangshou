const assert = require('assert');
const fs = require('fs');
const path = require('path');

let saved = [{
  id: 'legacy-hello', name: '你好', action: '你好', text: '你好', category: 'translation',
  fingers: [10, 20, 20, 95, 96, 20, 20, 20, 20, 20],
  states: ['straight', 'straight', 'straight', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight'],
  pose: null, enabled: true, createdAt: '2020-01-01T00:00:00.000Z', updatedAt: '2020-01-02T00:00:00.000Z'
}];
const navigation = [];
const toasts = [];
const bindingReferences = new Map([['legacy-hello', 'existing-control-binding']]);
const runtimeState = {
  connected: true,
  connecting: false,
  statusText: '数据已连接',
  flex: [99, 20, 20, 94, 95, 20, 20, 20, 20, 20],
  fingerStates: ['full', 'straight', 'straight', 'full', 'full', 'straight', 'straight', 'straight', 'straight', 'straight'],
  pose: { roll: 0, pitch: 0, yaw: 0 },
  calibration: { enabledFingers: [false, false, false, true, true, false, false, false, false, false] }
};
const runtime = {
  subscribe(listener) { listener(runtimeState); return () => {}; },
  getState() { return runtimeState; }
};

global.wx = {
  getStorageSync() { return saved; },
  setStorageSync(key, value) { saved = value; },
  showToast(options) { toasts.push(options); },
  navigateTo(options) { navigation.push(options); },
  showModal() {}
};
global.getApp = () => ({ getRuntime: () => runtime });

let definition;
global.Page = (value) => { definition = value; };
require('../pages/gesture-library/gesture-library');
const library = {
  ...definition,
  data: JSON.parse(JSON.stringify(definition.data)),
  setData(patch) { Object.assign(this.data, patch); }
};
library.onLoad();
library.onShow();
assert.strictEqual(library.data.items[0].needsResample, true, '旧库页面应展示需重采样状态');
const libraryMarkup = fs.readFileSync(path.join(__dirname, '../pages/gesture-library/gesture-library.wxml'), 'utf8');
assert.match(libraryMarkup, /需重新采样/);
assert.match(libraryMarkup, /重新采样并替换/);
assert.strictEqual(typeof library.resample, 'function', '旧模板应提供重采样入口');
library.resample({ currentTarget: { dataset: { id: 'legacy-hello' } } });
assert.match(navigation[0].url, /gesture-train.*replaceId=legacy-hello/);

global.Page = (value) => { definition = value; };
require('../pages/gesture-train/gesture-train');
const trainer = {
  ...definition,
  data: JSON.parse(JSON.stringify(definition.data)),
  setData(patch) { Object.assign(this.data, patch); }
};
trainer.onLoad({ replaceId: 'legacy-hello' });
assert.strictEqual(trainer.data.isResampling, true);
assert.strictEqual(trainer.data.name, '你好');
trainer.save();

assert.strictEqual(saved.length, 1, '重采样应替换原记录而不是新增副本');
assert.strictEqual(saved[0].id, 'legacy-hello', '重采样需保留绑定引用使用的模板 ID');
assert.deepStrictEqual(saved[0].enabledFingers, runtimeState.calibration.enabledFingers, '保存采样时十个启用位');
assert.strictEqual(bindingReferences.get(saved[0].id), 'existing-control-binding', '原 ID 对应的控制绑定继续有效');
assert.strictEqual(library.store.list()[0].needsResample, false, '替换后旧库状态应解除');
assert.ok(toasts.some((item) => item.title === '手势已替换'));

trainer.onUnload();
console.log('gesture page tests passed');
