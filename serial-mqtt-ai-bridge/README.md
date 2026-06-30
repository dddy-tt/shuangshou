# serial-mqtt-ai-bridge

`serial-mqtt-ai-bridge` 是双手智能手语交互手套系统的本地桥接程序第一版。当前版本为纯 Mock 演示，用于后续衔接 STM32 串口数据、Web Dashboard WebSocket、MQTT 继电器板和 AI 训练反馈服务。

## 当前能力

- 提供 HTTP 健康检查接口
- 提供 AI 康复训练辅助反馈接口
- 提供 WebSocket Mock 数据推送
- 预留 STM32 文本帧解析函数
- 预留 MQTT 控制发布接口

## 安装依赖

```bash
cd serial-mqtt-ai-bridge
npm install
```

## 配置环境变量

先复制示例配置：

```bash
copy .env.example .env
```

当前不需要填写真实 API Key。请不要提交 `.env`。

## 启动命令

```bash
npm start
```

默认启动地址：

- HTTP: `http://localhost:8765`
- WebSocket: `ws://localhost:8765`

## 健康检查

访问：

```bash
curl http://localhost:8765/health
```

预期返回：

```json
{
  "ok": true,
  "service": "serial-mqtt-ai-bridge",
  "mode": "mock"
}
```

## AI feedback API

请求示例：

```bash
curl -X POST http://localhost:8765/api/ai-feedback ^
  -H "Content-Type: application/json" ^
  -d "{\"targetGesture\":\"RIGHT_OPEN\",\"actualGesture\":\"RIGHT_FIST\",\"isCorrect\":false,\"confidence\":68,\"holdMs\":700,\"userLevel\":\"beginner\"}"
```

返回示例：

```json
{
  "feedback": "这次识别到的动作和目标不一致，请放慢动作，注意手指伸展幅度，再尝试一次。"
}
```

## WebSocket Mock 推送

网页或本地客户端连接 `ws://localhost:8765` 后，服务端会每 3 秒推送一条 Mock 消息，轮流发送以下三类数据：

1. `gesture`
2. `sign`
3. `care`

## 当前说明

- 当前是 Mock 第一版，不连接真实 STM32、MQTT 或 DeepSeek
- MQTT 先保留 publish 接口结构
- 串口帧解析已预留 `frameParser.js`

## 后续计划

- 接入真实 STM32 串口/蓝牙数据
- 接入真实 MQTT 设备控制
- 接入真实 DeepSeek API
- 与 `web-dashboard/` 做联调
