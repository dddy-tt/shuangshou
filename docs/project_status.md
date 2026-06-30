# 项目当前状态记录

本文档用于记录 `shuangshou` 仓库当前较稳定的项目状态，便于后续继续开发、联调与交接。

## 1. 当前项目目录结构

仓库根目录当前主要包括以下部分：

- `shuangshou/`
  - STM32F407ZGT6 固件工程
  - 含 CubeMX、HAL、Keil MDK 工程文件
- `web-dashboard/`
  - 前端演示看板
  - 基于 `Vite + React + TypeScript + Tailwind CSS`
- `serial-mqtt-ai-bridge/`
  - 本地桥接后端
  - 基于 `Node.js + Express + ws`
- `docs/`
  - 项目说明、联调勾选表、当前状态记录等文档

## 2. 已完成模块

当前已完成并可独立运行或验证的模块包括：

- STM32 固件主链路静态整理
  - `JY61P + Flex + Gesture + MAX30102 + BT + DFPlayer`
- `bringup_diag` 上电诊断输出
- `flex_sensor` DMA 快照有效性保护
- `web-dashboard` 四模块前端演示版
  - 手语翻译
  - AI 康复训练
  - 护理监测
  - 家电远控
- `serial-mqtt-ai-bridge` Mock 第一版
  - HTTP `/health`
  - HTTP `/api/ai-feedback`
  - WebSocket Mock 推送
  - `frameParser.js` 文本帧解析
  - `mqttClient.js` Mock publish
- `web-dashboard` 已完成对 `serial-mqtt-ai-bridge` 的优先接入与 Mock 回退
- `serial-mqtt-ai-bridge` 已完成 DeepSeek API 可选接入与自动回退

## 3. STM32 固件状态

当前 STM32 固件侧状态如下：

- 主控：`STM32F407ZGT6`
- 固件目录：`shuangshou/`
- 当前主链路保持为：
  - `JY61P + Flex + Gesture + MAX30102 + BT + DFPlayer`
- `bringup_diag` 已接入 `main.c`
  - 可输出右/左 JY61P 状态
  - 可输出 JY61P 初始化返回码
  - 可输出 MAX / ADC1 / ADC2 / DEG 诊断字段
- `flex_sensor` 已做 DMA 快照有效性保护
- Keil ARMCC V5.05 已验证通过
  - 最近一次已确认 `0 Error(s), 0 Warning(s)`
- 当前仍处于“无实物硬件优先收口阶段”
  - 目标是可编译、可接线、可联调
  - 暂未进入真实硬件数据闭环联调

## 4. web-dashboard 状态

当前前端状态如下：

- 目录：`web-dashboard/`
- 技术栈：
  - `Vite`
  - `React`
  - `TypeScript`
  - `Tailwind CSS`
- 页面当前包含四个功能模块：
  - 手语翻译
  - AI 康复训练
  - 护理监测
  - 家电远控
- 前端支持两种运行模式：
  - 纯前端 Mock 模式
  - bridge 在线优先接入模式
- 当前已实现：
  - WebSocket 优先连接 `ws://localhost:8765`
  - bridge 在线时接收 `gesture / sign / care / system`
  - bridge 不在线时自动回退本地 Mock
  - AI 反馈优先请求 bridge 的 `/api/ai-feedback`
  - bridge 请求失败时自动回退前端本地反馈
- 当前家电远控仍为前端状态切换演示
  - 未接真实 MQTT

## 5. serial-mqtt-ai-bridge 状态

当前 bridge 状态如下：

- 目录：`serial-mqtt-ai-bridge/`
- 技术栈：
  - `Node.js`
  - `Express`
  - `ws`
  - `dotenv`
  - `cors`
- 默认服务地址：
  - HTTP：`http://localhost:8765`
  - WebSocket：`ws://localhost:8765`
- 当前已提供能力：
  - `GET /health`
  - `POST /api/ai-feedback`
  - WebSocket 每 3 秒推送 Mock 消息
  - `frameParser.js` 解析 `BRINGUP / GESTURE / CTRL`
  - `mqttClient.js` 提供 Mock publish 结构
- 当前仍未接入：
  - 真实 STM32 串口
  - 真实 MQTT 设备
  - 真实蓝牙串口链路

## 6. DeepSeek 接入状态

当前 DeepSeek 接入状态如下：

- DeepSeek 接入位置：
  - `serial-mqtt-ai-bridge/src/aiFeedback.js`
- 调用方式：
  - 通过 OpenAI 兼容 `Chat Completions` 接口调用
- 环境变量支持：
  - `DEEPSEEK_API_KEY`
  - `DEEPSEEK_BASE_URL`
  - `DEEPSEEK_MODEL`
  - `DEEPSEEK_TIMEOUT_MS`
- 当前行为：
  - 有 `DEEPSEEK_API_KEY` 时优先请求真实 DeepSeek API
  - 无 `DEEPSEEK_API_KEY` 时直接回退本地 Mock
  - 超时、HTTP 非 2xx、返回为空或异常时自动回退本地 Mock
- 当前返回格式：
  - `{ "feedback": "...", "source": "deepseek" }`
  - 或 `{ "feedback": "...", "source": "mock" }`
- 当前未在仓库中保存任何真实 API Key

## 7. 本地运行命令

### 7.1 仅运行前端 Mock

```bash
cd web-dashboard
npm install
npm run dev
```

访问：

```text
http://localhost:3000
```

### 7.2 运行 bridge + dashboard 联调

终端 1：

```bash
cd serial-mqtt-ai-bridge
npm install
npm start
```

终端 2：

```bash
cd web-dashboard
npm install
npm run dev
```

访问：

```text
http://localhost:3000
```

### 7.3 测试 bridge 健康接口

```bash
curl http://localhost:8765/health
```

### 7.4 测试 AI feedback 接口

```bash
curl -X POST http://localhost:8765/api/ai-feedback ^
  -H "Content-Type: application/json" ^
  -d "{\"targetGesture\":\"RIGHT_OPEN\",\"actualGesture\":\"RIGHT_FIST\",\"isCorrect\":false,\"confidence\":68,\"holdMs\":700,\"userLevel\":\"beginner\"}"
```

## 8. 下一步计划

后续建议按以下顺序继续推进：

### 8.1 MQTT 接入

- 将 `serial-mqtt-ai-bridge/src/mqttClient.js` 从 Mock publish 升级为真实 MQTT 客户端
- 接入实际 broker 与家电控制主题
- 保留前端按钮状态切换，同时增加 bridge 转发控制能力

### 8.2 STM32 串口接入

- 在 `serial-mqtt-ai-bridge/` 中补串口读取逻辑
- 对接 STM32 蓝牙/串口输出文本帧
- 复用当前 `frameParser.js` 解析：
  - `BRINGUP`
  - `GESTURE`
  - `CTRL`
- 将解析后的数据转发到 WebSocket，替换部分 Mock 数据

### 8.3 硬件联调

- 确认 JY61P 已切到 I2C 模式
- 验证 ADC1 / ADC2 DMA 快照是否正常
- 验证 `BRINGUP` 串口诊断是否稳定
- 验证蓝牙串口链路与 bridge 是否连通
- 验证 DFPlayer、蓝牙调试与现有串口逻辑是否共存
- 在真实硬件到位后，逐步从 Mock 切换到真实数据链路

## 9. 当前结论

截至目前，项目已经形成“三段式”稳定结构：

- STM32 固件侧：可编译、已完成关键静态收口
- bridge 后端侧：可本地运行，支持 Mock、DeepSeek 可选接入与自动回退
- dashboard 前端侧：可独立运行，也可接入 bridge 联调

当前最值得推进的下一阶段工作，是 MQTT 接入、STM32 串口接入和真实硬件联调。
