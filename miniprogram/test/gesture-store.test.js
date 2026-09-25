const assert = require('assert');
const { createGestureStore } = require('../services/gesture-store');

let saved = [{
  id: 'original', name: '旧手势', text: '你好', action: '你好', fingers: Array(10).fill(20), states: [], pose: null,
  createdAt: '2020-01-01T00:00:00.000Z', updatedAt: '2020-01-02T00:00:00.000Z'
}];
const store = createGestureStore({ read: () => saved, write: (next) => { saved = next; } });

assert.strictEqual(store.list()[0].updatedAt, '2020-01-02T00:00:00.000Z');
assert.strictEqual(store.list()[0].needsResample, true, '没有采样掩码的旧模板必须标记为需重采样');
assert.strictEqual(Object.prototype.hasOwnProperty.call(store.list()[0], 'enabledFingers'), false, '不能猜测旧模板的采样掩码');
store.update('original', { text: '您好', action: '您好' });
assert.strictEqual(store.list()[0].createdAt, '2020-01-01T00:00:00.000Z');
assert.notStrictEqual(store.list()[0].updatedAt, '2020-01-02T00:00:00.000Z');

const replacementMask = [false, false, false, true, true, false, false, false, false, false];
store.update('original', {
  fingers: Array(10).fill(85),
  states: Array(10).fill('full'),
  enabledFingers: replacementMask
});
assert.strictEqual(store.list()[0].id, 'original', '重采样应原位保留模板 ID');
assert.deepStrictEqual(store.list()[0].enabledFingers, replacementMask);
assert.strictEqual(store.list()[0].needsResample, false, '提供有效掩码后应解除旧模板提示');

store.add({
  id: 'new-safe', name: '新模板', action: '测试', fingers: Array(10).fill(20), states: Array(10).fill('straight'),
  enabledFingers: Array(10).fill(true)
});
assert.strictEqual(store.list().find((item) => item.id === 'new-safe').needsResample, false);

const backup = JSON.stringify({ items: [{ id: 'imported', name: '导入手势', text: '再见', fingers: Array(10).fill(30), states: [], pose: null }] });
store.importJson(backup);
store.importJson(backup);
assert.deepStrictEqual(store.list().map((item) => item.id).sort(), ['imported', 'new-safe', 'original']);
assert.strictEqual(store.list().find((item) => item.id === 'imported').needsResample, true, '无掩码的兼容导入应标记需重采样');
console.log('gesture store tests passed');
