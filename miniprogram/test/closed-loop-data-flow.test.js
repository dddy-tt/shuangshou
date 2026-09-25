const assert = require('assert');
const { createAppState } = require('../store/app-state');
const { STORAGE_KEY } = require('../services/gesture-store');

const storage = {};
let definition;
let now = 10000;
let spoken = [];
const originalNow = Date.now;

global.wx = {
  getStorageSync(key) { return storage[key]; },
  setStorageSync(key, value) { storage[key] = value; }
};
global.Page = (value) => { definition = value; };
Date.now = () => now;

const gesture = {
  id: 'closed-loop-straight',
  name: '闭环伸直手势',
  action: '闭环测试语音',
  text: '闭环测试语音',
  category: 'translation',
  fingers: Array(10).fill(10),
  states: Array(10).fill('straight'),
  enabledFingers: Array(10).fill(true),
  enabled: true
};
storage[STORAGE_KEY] = [gesture];

let bluetoothOperations = 0;
const runtime = createAppState({
  bluetoothFactory: () => ({
    startDiscovery() { bluetoothOperations += 1; },
    connect() { bluetoothOperations += 1; },
    disconnect() { bluetoothOperations += 1; },
    closeAdapter() { bluetoothOperations += 1; },
    write() { bluetoothOperations += 1; }
  })
});
global.getApp = () => ({ getRuntime: () => runtime });
require('../pages/translation/translation');

const page = {
  ...definition,
  data: { ...definition.data },
  setData(patch) { Object.assign(this.data, patch); }
};

async function main() {
try {
  page.onLoad();
  page.onShow();
  // This fixture represents a newly captured template; separate store/page
  // tests cover normalization and legacy resampling behavior.
  page.gestures = [gesture];
  page.tts.speak = (text) => {
    spoken.push(text);
    return Promise.resolve({ ok: true });
  };

  const flexFrame = 'FLEX|L1=10|L2=10|L3=10|L4=10|L5=10|R1=10|R2=10|R3=10|R4=10|R5=10\r\n';
  runtime.ingestRawData(Buffer.from(flexFrame));
  runtime.ingestRawData(Buffer.from('IMU|R=10.00|P=-5.00|Y=2.00\r\n'));
  runtime.ingestRawData(Buffer.from('ACC|X=2.50|Y=-2.50|Z=1.00|VALID=1\r\n'));
  await new Promise((resolve) => setTimeout(resolve, 45));

  assert.strictEqual(runtime.getState().connected, false, '闭环测试不应建立 BLE 连接');
  assert.deepStrictEqual(runtime.getState().flex, Array(10).fill(10), 'FLEX 文本帧应经过协议解析进入 app-state');
  assert.deepStrictEqual(runtime.getState().pose, { roll: 10, pitch: -5, yaw: 2 }, 'IMU 文本帧应进入姿态状态');
  assert.deepStrictEqual(
    { x: runtime.getState().acc.x, y: runtime.getState().acc.y, z: runtime.getState().acc.z, valid: runtime.getState().acc.valid },
    { x: 2.5, y: -2.5, z: 1, valid: true },
    'ACC 文本帧应进入诊断状态'
  );
  assert.strictEqual(runtime.getState().alarm.active.length, 0, 'ACC 数值不能由小程序自行伪造固件报警事件');

  now += 400;
  runtime.ingestRawData(Buffer.from(flexFrame));
  await new Promise((resolve) => setTimeout(resolve, 45));
  assert.deepStrictEqual(spoken, ['闭环测试语音'], `静止手势应通过正式识别链路播报：${JSON.stringify({ recognition: runtime.getState().recognition, pageRecognition: page.data.recognition, translation: page.data.translation, lastFlexAt: runtime.getState().lastFlexAt, lastMatchedFlexAt: page.lastMatchedFlexAt, flex: runtime.getState().flex, enabledFingers: runtime.getState().calibration.enabledFingers, gestures: page.gestures })}`);
  assert.strictEqual(bluetoothOperations, 0, '闭环流程不得调用任何 BLE 连接或写入 API');

  page.onHide();
  page.onUnload();
  console.log('closed-loop data flow passed without BLE');
} finally {
  if (page.poseTimer) page.stopPoseRenderer();
  if (page.unsubscribe) page.unsubscribe();
  if (page.tts) page.tts.stop();
  Date.now = originalNow;
  delete global.wx;
  delete global.Page;
  delete global.getApp;
}
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
