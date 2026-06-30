# web-dashboard

`web-dashboard` 是双手智能手语交互手套系统的前端演示看板，使用 `Vite + React + TypeScript + Tailwind CSS` 构建。

## 当前说明

- 当前版本支持四个模块：
  - 手语翻译
  - AI 康复训练
  - 护理监测
  - 家电远控
- 默认仍可纯前端 Mock 运行
- 如果本地 `serial-mqtt-ai-bridge` 已启动，页面会优先接入 bridge 的 WebSocket 和 AI feedback 接口
- bridge 不在线时会自动回退到前端 Mock
- 不包含真实 API Key
- 不接真实 DeepSeek、STM32 串口或 MQTT

## 运行方式

### 方式 A：纯前端 Mock

```bash
cd web-dashboard
npm install
npm run dev
```

访问地址：

```text
http://localhost:3000
```

### 方式 B：连接 bridge

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

访问地址：

```text
http://localhost:3000
```

## 构建命令

```bash
npm run build
```

## 当前前端行为

- AI 康复训练
  - 点击“开始训练采集”后开始倒计时
  - bridge 在线时，等待下一条 `gesture` 消息作为训练结果
  - bridge 不在线时，回退前端本地模拟结果
  - AI 反馈优先请求 `http://localhost:8765/api/ai-feedback`
  - 如果请求失败，自动回退本地模拟反馈
- 手语翻译
  - bridge 在线时接收 `sign` 消息
  - bridge 不在线时可继续使用本地 Mock
- 护理监测
  - bridge 在线时接收 `care` 消息
  - bridge 不在线时继续展示本地 Mock
- 家电远控
  - 当前仍为前端状态切换演示

## 后续接入方向

- 接入真实 STM32 串口 / 蓝牙上报
- 接入真实 MQTT 设备控制
- 接入真实 DeepSeek 服务
