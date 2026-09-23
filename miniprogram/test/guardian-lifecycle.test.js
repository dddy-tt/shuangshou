const assert = require('assert');

let definition;
global.App = (value) => { definition = value; };
require('../app');

let showCount = 0;
let hideCount = 0;
const app = {
  runtime: {
    onAppShow() { showCount += 1; return Promise.resolve(); },
    onAppHide() { hideCount += 1; }
  }
};

assert.strictEqual(typeof definition.onShow, 'function');
assert.strictEqual(typeof definition.onHide, 'function');
definition.onShow.call(app);
definition.onHide.call(app);
assert.strictEqual(showCount, 1, '小程序回到前台必须触发 runtime 恢复流程');
assert.strictEqual(hideCount, 1, '小程序进入后台必须触发 runtime 停止流程');

console.log('guardian lifecycle tests passed');
