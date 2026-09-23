const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const app = JSON.parse(fs.readFileSync(path.join(root, 'app.json'), 'utf8'));
const pageJs = fs.readFileSync(path.join(root, 'pages/simulator/simulator.js'), 'utf8');
const pageWxml = fs.readFileSync(path.join(root, 'pages/simulator/simulator.wxml'), 'utf8');
const settingsJs = fs.readFileSync(path.join(root, 'pages/settings/settings.js'), 'utf8');
const settingsWxml = fs.readFileSync(path.join(root, 'pages/settings/settings.wxml'), 'utf8');

assert.ok(app.pages.includes('pages/simulator/simulator'), 'Simulator 页面必须注册为非 TabBar 页面');
assert.ok(!app.tabBar.list.some((item) => item.pagePath === 'pages/simulator/simulator'), 'Simulator 不得进入正式底部导航');
assert.doesNotMatch(settingsJs, /isDeveloperTools/, '入口不能依赖不稳定的开发者工具平台字符串');
assert.match(settingsJs, /goSimulator/);
assert.match(settingsWxml, /开发测试功能/);
assert.match(settingsWxml, /bindtap="goSimulator"/);
assert.doesNotMatch(settingsWxml, /wx:if="\{\{showSimulator\}\}"/, '入口必须始终显示');
assert.match(pageJs, /setSimulatorSnapshot/);
assert.match(pageJs, /applySimulatorPreset/);
assert.match(pageJs, /startSimulator/);
assert.match(pageJs, /stopSimulator/);
assert.match(pageWxml, /bindchanging="updateFinger"/);
assert.match(pageWxml, /bindchanging="updateImu"/);
assert.match(pageWxml, /bindchanging="updateAcc"/);
console.log('simulator page tests passed');
