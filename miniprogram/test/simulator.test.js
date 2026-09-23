const assert = require('assert');
const helloCase = require('./cases/hello.json');
const { createProtocolParser } = require('../utils/protocol');
const { classifyFingers } = require('../services/gesture-matcher');
const { createSimulator, buildTelemetryFrames } = require('../services/simulator');
const { createAppState } = require('../store/app-state');

function createFrameCollector() {
  const frames = [];
  const errors = [];
  const parser = createProtocolParser({
    onFrame: (frame) => frames.push(frame),
    onError: (error) => errors.push(error.message)
  });
  return { frames, errors, push: (arrayBuffer) => parser.push(arrayBuffer) };
}

function fakeBluetooth() {
  return {
    scanCalls: 0,
    connectCalls: 0,
    startDiscovery() { this.scanCalls += 1; return Promise.resolve(); },
    connect() { this.connectCalls += 1; return Promise.resolve(); },
    disconnect: () => Promise.resolve(),
    closeAdapter: () => Promise.resolve(),
    write: () => Promise.resolve()
  };
}

function main() {
  // 同一批虚拟文本帧按 7 字节分包后，仍必须经过真实 parser 正确还原。
  const collector = createFrameCollector();
  let clock = 100;
  let scheduledTick = null;
  let intervalLive = false;
  const simulator = createSimulator({
    now: () => clock,
    onData: collector.push,
    setInterval: (callback) => { scheduledTick = callback; intervalLive = true; return 1; },
    clearInterval: () => { intervalLive = false; }
  });
  simulator.loadCase(helloCase, { inject: false });
  simulator.inject({ includeBoot: true, includeStatus: true, chunkBytes: 7 });
  assert.deepStrictEqual(collector.errors, []);
  assert.deepStrictEqual(collector.frames.map((frame) => frame.type), ['boot', 'bringup', 'jy', 'flex', 'imu', 'acc']);
  assert.deepStrictEqual(collector.frames[3].left, helloCase.flex.slice(0, 5));
  assert.deepStrictEqual(collector.frames[3].right, helloCase.flex.slice(5));
  assert.deepStrictEqual(collector.frames[4], { type: 'imu', roll: 8, pitch: -4, yaw: 16 });
  assert.deepStrictEqual(collector.frames[5], { type: 'acc', x: 0.05, y: -0.02, z: 0.99, valid: true, unit: 'g' });

  // 预设不包含识别结果；它只能生成传感器数据。
  assert.deepStrictEqual(buildTelemetryFrames({ flex: Array(10).fill(50), imu: {}, acc: {} }).map((line) => line.split('|')[0]), ['FLEX', 'IMU', 'ACC']);
  simulator.start();
  assert.strictEqual(simulator.getState().running, true);
  clock += 250;
  if (intervalLive) scheduledTick();
  const injectedAt = simulator.getState().lastInjectedAt;
  simulator.stop();
  assert.strictEqual(simulator.getState().running, false);
  assert.strictEqual(simulator.getState().active, false);
  clock += 250;
  if (intervalLive) scheduledTick();
  assert.strictEqual(simulator.getState().lastInjectedAt, injectedAt, '停止后不得继续产生数据帧');

  // app-state 只能从 Simulator 的 ArrayBuffer 帧更新，而不是页面直接写状态。
  const bluetooth = fakeBluetooth();
  const runtime = createAppState({ bluetooth });
  runtime.loadSimulatorCase(helloCase, { chunkBytes: 5, includeBoot: true, includeStatus: true });
  const state = runtime.getState();
  assert.deepStrictEqual(state.rawFlex, helloCase.flex);
  assert.deepStrictEqual(state.flex, helloCase.flex);
  assert.deepStrictEqual(state.fingerStates, ['straight', 'straight', 'half', 'full', 'full', 'straight', 'half', 'half', 'full', 'full']);
  assert.deepStrictEqual(state.rawPose, helloCase.imu);
  assert.deepStrictEqual(state.acc, { type: 'acc', x: 0.05, y: -0.02, z: 0.99, valid: true, unit: 'g', lastAt: state.acc.lastAt });
  assert.ok(state.acc.lastAt > 0);
  assert.strictEqual(state.jyStatus.online, true);
  assert.strictEqual(state.jyValid, true);
  assert.strictEqual(state.connected, false, 'Simulator 不能伪造 BLE 连接状态');
  assert.strictEqual(state.lastError, '', '不连接 BLE 启动 Simulator 不能产生 BLE 错误');
  assert.strictEqual(bluetooth.scanCalls, 0);
  assert.strictEqual(bluetooth.connectCalls, 0);
  assert.deepStrictEqual(classifyFingers([0, 29.999, 30, 69.999, 70, 0, 30, 70, 100, 50]), ['straight', 'straight', 'half', 'half', 'full', 'straight', 'half', 'full', 'full', 'half']);

  runtime.startSimulator();
  assert.strictEqual(runtime.getState().simulator.running, true);
  runtime.stopSimulator();
  assert.strictEqual(runtime.getState().simulator.running, false);
  assert.strictEqual(runtime.getState().simulator.active, false);
  console.log('simulator tests passed');
}

main();
