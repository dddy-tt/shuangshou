const assert = require('assert');
const fs = require('fs');

const translation = fs.readFileSync('miniprogram/pages/translation/translation.wxml', 'utf8');
const rehabilitation = fs.readFileSync('miniprogram/pages/rehabilitation/rehabilitation.wxml', 'utf8');
const alarm = fs.readFileSync('miniprogram/pages/alarm/alarm.wxml', 'utf8');
const alarmPage = fs.readFileSync('miniprogram/pages/alarm/alarm.js', 'utf8');
const home = fs.readFileSync('miniprogram/pages/home/home.wxml', 'utf8');
const homePage = fs.readFileSync('miniprogram/pages/home/home.js', 'utf8');
const settings = fs.readFileSync('miniprogram/pages/settings/settings.js', 'utf8');
const app = fs.readFileSync('miniprogram/app.js', 'utf8');
const project = JSON.parse(fs.readFileSync('miniprogram/project.config.json', 'utf8'));

assert.match(translation, /查看报警/);
assert.match(translation, /alarmCopy/);
assert.doesNotMatch(translation, /safetyAlert\.axis/);
assert.match(rehabilitation, /ACTIVE=0/);
assert.match(rehabilitation, /resolveText/);
assert.doesNotMatch(rehabilitation, /safetyAlert\.axis/);
assert.match(alarm, /确认并停止本地提示/);
assert.match(alarm, /resolveText/);
assert.match(alarm, /disabled="\{\{item\.resolveDisabled\}\}"/);
assert.match(alarm, /resolveStatusText/);
assert.match(alarmPage, /等待设备回执/);
assert.match(home, /隐藏本机旧提示/);
assert.match(home, /canHideLocalAlarm/);
assert.match(homePage, /handleHideLocalAlarm/);
assert.match(homePage, /hideLocalAlarm/);
assert.match(settings, /传感器离线/);
assert.match(settings, /数据过期/);
assert.match(settings, /JY\|ONLINE/);
assert.match(app, /onShow\(\)/);
assert.match(app, /onHide\(\)/);
assert.ok(project.packOptions.ignore.some((item) => item.type === 'folder' && item.value === 'cloudfunctions'));
assert.ok(project.packOptions.ignore.some((item) => item.type === 'folder' && item.value === 'device-firmware'));

console.log('alarm page/config tests passed');
