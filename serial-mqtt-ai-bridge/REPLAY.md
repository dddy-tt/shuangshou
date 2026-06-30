# 数据录制与回放说明

## 本次新增文件

- `src/framePipeline.js`
  - 统一负责文本帧进入 `frameParser` 前的日志记录
  - 负责从 `frame_log.jsonl` 读取并按原始时间间隔回放
- `logs/.gitignore`
  - 忽略运行时生成的 `logs/frame_log.jsonl`

## 录制规则

所有进入 `frameParser` 的文本帧，都会被记录到：

```text
logs/frame_log.jsonl
```

每一行都是一条 JSON，字段包括：

- `timestamp`
- `source`
- `rawFrame`

示例：

```json
{"timestamp":1710000000000,"source":"mock","rawFrame":"GESTURE:ID=RIGHT_OPEN,CONF=86,HOLD=1200"}
```

当前来源标记约定：

- `mock`
- `ws`
- `serial`

说明：

- `POST /api/parse-frame` 默认按 `ws` 来源记录
- Mock 模式下生成的手势文本帧会按 `mock` 记录
- Replay 会重用已有日志回放，不会把同一批日志再次追加回原文件

## 回放模式

当设置：

```env
USE_REPLAY=true
```

启动后会读取：

```env
FRAME_LOG_PATH=logs/frame_log.jsonl
```

然后按每条日志原本的时间间隔重新送入 `frameParser`。

## 输入源优先级

固定优先级如下：

```text
replay > serial > mock
```

当前状态：

- `USE_REPLAY=true` 时，直接走 replay
- `USE_REPLAY=false` 且 `MODE=serial` 时，进入串口优先分支
- 串口真实接入当前仍是预留状态，会提示后回退到 Mock
- 其余情况使用 Mock

## 运行方式

### 1. Mock 模式

```bash
cd serial-mqtt-ai-bridge
copy .env.example .env
npm install
npm start
```

推荐 `.env`：

```env
MODE=mock
USE_REPLAY=false
FRAME_LOG_PATH=logs/frame_log.jsonl
```

### 2. Replay 模式

先准备已有日志文件 `logs/frame_log.jsonl`，然后设置：

```env
USE_REPLAY=true
FRAME_LOG_PATH=logs/frame_log.jsonl
```

启动：

```bash
npm start
```

### 3. 串口预留模式

```env
MODE=serial
USE_REPLAY=false
```

当前会进入串口优先分支，但因为真实串口逻辑尚未实现，最终会回退到 Mock。
