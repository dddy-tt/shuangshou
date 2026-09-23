const assert = require('assert');
const fs = require('fs');

const homeStyles = fs.readFileSync('miniprogram/pages/home/home.wxss', 'utf8');
const settingsStyles = fs.readFileSync('miniprogram/pages/settings/settings.wxss', 'utf8');
assert.match(homeStyles, /\.quick-grid\s*\{[^}]*display:\s*flex[^}]*flex-wrap:\s*wrap/s);
assert.match(homeStyles, /\.quick-item\s*\{[^}]*width:\s*48%[^}]*min-width:\s*0/s);
assert.doesNotMatch(homeStyles, /\.quick-grid\s*\{[^}]*display:\s*grid/s);
assert.match(settingsStyles, /\.cal-grid\s*\{[^}]*display:\s*block[^}]*width:\s*100%/s);
assert.match(settingsStyles, /\.cal-grid button\s*\{[^}]*display:\s*flex\s*!important[^}]*width:\s*100%\s*!important/s);
assert.match(settingsStyles, /\.cal-grid button \+ button\s*\{[^}]*margin-top:\s*14rpx/s);
console.log('layout guard tests passed');
