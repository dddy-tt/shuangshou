# 双手手语康复系统项目交接总览

本文档用于给新的对话窗口快速交接当前仓库状态，减少上下文过长带来的误判。以下内容以 2026-06-30 当前仓库实况为准。

## 1. 项目目标

当前项目是一个由三部分组成的联动系统：

1. `shuangshou/`
   STM32F407ZGT6 固件工程，负责手套侧传感器采集、手势识别、蓝牙/串口输出、DFPlayer 语音等底层能力。
2. `web-dashboard/`
   React + Vite 前端演示看板，负责展示手语翻译、训练反馈、护理监测和家电远控。
3. `serial-mqtt-ai-bridge/`
   Node.js 本地桥接服务，负责连接 Dashboard、后续 STM32 串口/蓝牙、后续 MQTT，以及 DeepSeek AI 反馈。

当前阶段目标不是大扩功能，而是先把整条链路收口到：

- 可编译
- 可启动
- 可演示
- 可在后续接硬件时快速联调

## 2. 当前目录结构

仓库根目录当前主要包含：

- `shuangshou/`
- `web-dashboard/`
- `serial-mqtt-ai-bridge/`
- `docs/`
- `start-demo.bat`
- `README.md`
- 若干历史设计说明和交接提示文件

## 3. 已完成内容

### 3.1 STM32 固件侧

当前主链路已经固定为：

- `JY61P + Flex + Gesture + MAX30102 + BT + DFPlayer`

已经完成的固件收口：

- `bringup_diag` 模块已接入 `main.c`
- `BRINGUP` 串口诊断可输出右/左 JY61P 在线状态
- `BRINGUP` 串口诊断可输出右/左 JY61P 初始化返回码
- `BRINGUP` 串口诊断可输出 `MAX / ADC1 / ADC2 / DEG` 状态
- `flex_sensor` 已增加 DMA 快照有效性保护
- `soft_i2c.c` 注释已与当前 JY61P/MAX 拓扑对齐
- 乱码注释已做过一轮清理
- Keil 工程已纳入 `bringup_diag.c`
- 用户已确认 Keil ARMCC V5.05 编译通过：`0 Error(s), 0 Warning(s)`

当前仍保持的硬件约束：

- 右手 JY61P：`PB6/PB7`
- 左手 JY61P：`PB8/PB9`
- MAX30102：`PC6/PC7`
- 振动电机当前以 `.ioc + vibrator.c` 为准：`TIM4 -> PD12/PD13`

### 3.2 前端 Dashboard

`web-dashboard/` 已经完成并可运行，当前为四模块结构：

1. 手语翻译
2. AI 康复训练
3. 护理监测
4. 家电远控

已经完成的前端能力：

- 纯前端 Mock 可独立运行
- 支持连接 `ws://localhost:8765`
- 支持 bridge 不在线时自动回退本地 Mock
- 手语翻译模块可显示：
  - 当前识别手势
  - 翻译文本
  - 语音播报状态
  - 置信度
  - 最近识别记录
- AI 康复训练模块可实现：
  - 目标动作选择
  - 开始训练采集
  - 3 秒倒计时
  - 模拟识别结果
  - 正确/错误判断
  - AI 康复训练辅助反馈
  - 训练统计
  - 最近 10 条记录
- 护理监测模块可显示：
  - HR
  - SpO2
  - 跌倒状态
  - SOS 状态
  - 护理辅助提醒
- 家电远控模块保留前端状态切换：
  - 灯
  - 风扇
  - 插座
  - SOS

### 3.3 Bridge 后端

`serial-mqtt-ai-bridge/` 已经完成 Mock 第一版，并继续增强到可接 DeepSeek：

- `GET /health`
- `POST /api/ai-feedback`
- `POST /api/control`
- `POST /api/parse-frame`
- WebSocket 服务：`ws://localhost:8765`
- 每 3 秒推送 Mock 消息：
  - `gesture`
  - `sign`
  - `care`
- `frameParser.js` 可解析：
  - `BRINGUP`
  - `GESTURE`
  - `CTRL`
- `mqttClient.js` 目前为 mock publish 结构
- DeepSeek 已支持通过环境变量启用，失败自动回退 Mock

当前 bridge 还新增了一个重要能力：

- WebSocket 现在能接收前端发来的 JSON 消息
- 当收到 `type === "gesture"` 时，会调用现有 AI 反馈逻辑
- 然后回包：
  - `type: "ai_feedback"`
  - `source: "deepseek" | "mock"`
  - `result: "中文反馈内容"`

这意味着当前 AI 链路已经支持走 WebSocket 闭环，而不是只能走旧的 HTTP 反馈接口。

### 3.4 DeepSeek 接入状态

当前 DeepSeek 已经接入 `serial-mqtt-ai-bridge`，特点如下：

- 不在前端保存 API Key
- API Key 只从 bridge 的环境变量读取
- 使用 OpenAI 兼容 Chat Completions 调用
- 无 Key 时自动使用 Mock
- 请求失败、超时、异常时自动使用 Mock
- 输出受约束：
  - 中文
  - 短反馈
  - 避免“医疗诊断”“处方”等表述

### 3.5 演示启动器

根目录已新增：

- `start-demo.bat`

当前能力：

- 自动检查 `npm` 是否可用
- 自动切到仓库根目录执行
- 独立窗口启动 bridge
- 等待 3 秒
- 独立窗口启动 dashboard
- 自动打开浏览器 `http://localhost:3000`
- 输出 `[INFO] / [WARN] / [OK]`
- 失败时会停住窗口，避免一闪而过

## 4. 当前整体进度判断

如果按“演示链路”来分，当前进度大致可以理解为：

### 已完成

- STM32 固件静态收口
- Keil 编译通过
- Dashboard 四模块演示
- Bridge 本地运行
- Dashboard 与 Bridge WebSocket 联动
- DeepSeek 后端接入与自动回退
- Demo 启动器

### 未完成

- 真实 STM32 串口/蓝牙数据接入 bridge
- 真实 MQTT 继电器板接入
- 家电远控从前端状态切换升级为真实控制
- 基于真实硬件数据验证 JY61P / Flex / MAX30102 联调
- Web 端系统状态总览面板
- 更完整的异常联调日志与录制

## 5. 现在还剩哪些任务没做

建议把后续工作按优先级分成三层。

### 第一优先级：真实链路接入

1. 给 `serial-mqtt-ai-bridge` 接入真实串口读帧
2. 将 STM32 的 `BRINGUP / GESTURE / CTRL` 文本帧真正喂给 `frameParser.js`
3. 把解析后的真实数据转发给 Dashboard

### 第二优先级：真实控制接入

1. 将 `mqttClient.js` 从 Mock publish 升级到真实 MQTT 客户端
2. 家电远控页面按钮改为调用 bridge 控制接口
3. bridge 再统一发给实际继电器板

### 第三优先级：硬件联调

1. 确认 JY61P 已切到 I2C 模式
2. 验证 ADC1/ADC2 DMA 快照稳定
3. 验证 `BRINGUP` 串口输出稳定
4. 验证蓝牙/串口到 bridge 的链路
5. 验证 DFPlayer 与蓝牙调试串口共存
6. 用真实数据替换当前前端 Mock 展示

## 6. 当前最适合新对话接手的统筹建议

如果新对话窗口要继续接手，建议遵守下面的推进顺序：

1. 不再大改 STM32 主链路，优先保持固件稳定
2. 优先把 bridge 做成真实串口入口
3. 然后再做 MQTT 真接入
4. 最后再做硬件联调和前端状态细化

换句话说，当前最值得推进的核心主线是：

`STM32 文本帧 -> bridge 解析 -> WebSocket 转发 -> Dashboard 展示`

而不是继续扩很多前端页面或再做新的 Mock 模块。

## 7. 当前运行方式

### 7.1 只运行前端 Mock

```bash
cd web-dashboard
npm install
npm run dev
```

访问：

```text
http://localhost:3000
```

### 7.2 运行 bridge + dashboard

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

### 7.3 一键演示启动

Windows 下直接双击：

```text
start-demo.bat
```

## 8. 新对话窗口要特别注意的约束

后续继续开发时，建议默认遵守这些边界：

- 不要轻易改 `shuangshou/` 主链路逻辑
- 不要改 JY61P 协议
- 不要改 MAX30102 算法
- 不要改 ADC 通道顺序
- 不要重构蓝牙协议
- 不要删除 legacy 文件
- 优先做小 diff、短路径、高价值收口

## 9. 给下一个 GPT 的一句话结论

这个仓库目前已经具备：

- 固件可编译
- 前端可演示
- bridge 可运行
- DeepSeek 可回退
- demo 可一键启动

下一阶段最值得做的不是继续扩 UI，而是把 `serial-mqtt-ai-bridge` 真正接上 STM32 串口/蓝牙和 MQTT，把整条链路从 Mock 过渡到真实联调。
