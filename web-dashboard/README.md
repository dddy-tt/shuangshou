# web-dashboard

纯前端 Mock 演示版训练看板，使用 `Vite + React + TypeScript + Tailwind CSS` 构建。

## 说明

- 当前版本是无硬件 Mock 演示版
- 不接入真实后端
- 不包含 API Key
- 不接入真实 DeepSeek API
- 用于演示训练任务、识别结果、AI 康复训练辅助反馈、训练统计和家电远控交互

## 安装命令

```bash
npm install
```

## 启动命令

```bash
npm run dev
```

## 访问地址

```text
http://localhost:3000
```

## 构建命令

```bash
npm run build
```

## 当前可演示内容

- 训练看板 tab
  - 选择目标动作
  - 开始训练采集
  - 3 秒倒计时
  - 生成模拟识别结果
  - 显示正确/错误状态
  - 显示 AI 康复训练辅助反馈
  - 更新训练次数、正确次数、正确率、连续正确次数
  - 展示最近 10 条记录
- 家电远控 tab
  - 灯开关
  - 风扇开关
  - 插座开关
  - SOS 触发/取消

## 后续接入方向

后续将逐步接入：

- `serial-mqtt-ai-bridge`
- DeepSeek API
- STM32 串口数据

## 目录

```text
web-dashboard/
├── package.json
├── tailwind.config.js
├── postcss.config.js
├── vite.config.ts
├── index.html
├── README.md
└── src/
    ├── main.tsx
    ├── index.css
    ├── App.tsx
    ├── types/
    │   └── index.ts
    ├── hooks/
    │   └── useWebSocket.ts
    ├── services/
    │   └── aiService.ts
    └── components/
        ├── StatusCard.tsx
        ├── TaskCard.tsx
        ├── ResultCard.tsx
        ├── FeedbackCard.tsx
        ├── StatsCard.tsx
        └── IotCard.tsx
```
