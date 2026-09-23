# 微信小程序虚拟手套测试系统

## 目的与范围

Simulator 只用于微信开发者工具和自动化测试。它不扫描 BLE、不建立 JDY-23 连接、不修改 STM32 固件，也不伪造“已连接”状态。

它的职责是生成与现有 STM32 文本协议相同的原始帧，并交给已有解析器处理。

```text
虚拟控制台 / 测试案例
  -> services/simulator.js
  -> ArrayBuffer 文本帧
  -> store/app-state.js::ingestRawData()
  -> utils/protocol.js
  -> 共享状态
  -> 翻译 / 手势匹配 / 康复 / 远控页面
```

因此模拟模式与真实 BLE 模式从 `protocol.js` 起使用同一条业务链路；Simulator 不会直接调用页面 `setData()`，也不会直接设置识别结果。

## 开发者工具操作

1. 在微信开发者工具中打开 `miniprogram/`。
2. 不连接 STM32 或 JDY-23，进入“设备状态”。
3. 在“设备状态”页底部的“开发测试功能”区域，点“打开虚拟手套控制台”。该入口始终显示，但仅用于开发与自动化验证。
4. 使用预设，或拖动十指、IMU、ACC 滑杆。每次调整都会立即注入一批真实格式帧。
5. 点“开始连续模拟”后，FLEX/IMU/ACC 每 250ms 注入一次；BRINGUP/JY 健康帧每秒补发一次。点“停止连续模拟”后停止定时注入，正式页面保留最后一帧显示。
6. 切到“翻译”“AI康复”或“远控”，应看到同一份十指与姿态数据。手势是否识别仍由原有手势库和 matcher 决定。

当前预设：`all_straight`、`all_half`、`all_bent`、`mixed_fingers`、`normal_pose`。

## 自动测试案例接口

`services/simulator.js` 导出 `createSimulator()`、`buildTelemetryFrames()`、`normalizeSnapshot()` 和 `PRESETS`。

运行时也暴露以下开发接口：

```js
const runtime = getApp().getRuntime();
runtime.loadSimulatorCase({
  flex: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100],
  imu: { roll: 0, pitch: 0, yaw: 0 },
  acc: { x: 0, y: 0, z: 1 }
});
runtime.startSimulator();
runtime.stopSimulator();
```

后续 PC/Codex 自动测试可读取 JSON 后调用 `loadSimulatorCase()`。示例位于 `miniprogram/test/cases/hello.json`。该接口只接受 `flex`、`imu`、`acc` 原始数据，不接受“识别结果”字段。

## 当前验证边界

- 已通过 Node 协议/状态回归验证：分包、十指分类、IMU、ACC、JY/BRINGUP 健康帧、停止生命周期，以及 Simulator 不调用 BLE。
- 仍需在微信开发者工具实际预览一次，验证 WXML/WXSS 渲染、滑杆触摸与页面切换。
- 真机 BLE FFE0/FFE1、JDY-23 Notify、STM32 真实采样和 TTS/音频不属于本功能的替代验证，仍需要手机与硬件单独测试。
