const assert = require('assert');
const { createGestureStore } = require('../services/gesture-store');

let saved = [{
  id: 'original', name: '旧手势', text: '你好', action: '你好', fingers: Array(10).fill(20), states: [], pose: null,
  createdAt: '2020-01-01T00:00:00.000Z', updatedAt: '2020-01-02T00:00:00.000Z'
}];
const store = createGestureStore({ read: () => saved, write: (next) => { saved = next; } });

assert.strictEqual(store.list()[0].updatedAt, '2020-01-02T00:00:00.000Z');
store.update('original', { text: '您好', action: '您好' });
assert.strictEqual(store.list()[0].createdAt, '2020-01-01T00:00:00.000Z');
assert.notStrictEqual(store.list()[0].updatedAt, '2020-01-02T00:00:00.000Z');

const backup = JSON.stringify({ items: [{ id: 'imported', name: '导入手势', text: '再见', fingers: Array(10).fill(30), states: [], pose: null }] });
store.importJson(backup);
store.importJson(backup);
assert.deepStrictEqual(store.list().map((item) => item.id).sort(), ['imported', 'original']);
console.log('gesture store tests passed');
