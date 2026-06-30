# STM32 串口接入状态说明

## 本轮目标

本轮目标是在不破坏现有 mock / replay / websocket 能力的前提下，为 `serial-mqtt-ai-bridge` 增加真实 STM32 串口输入能力，并把输入优先级调整为：

```text
serial > replay > mock
```

## 本轮完成内容

### 1. 新增串口输入模块

新增文件：

- `src/serialInput.js`

职责：

- 打开串口
- 按行读取 STM32 输出文本
- 将 `|` 分隔格式适配成 `frameParser.js` 现有支持格式

### 2. 串口文本适配规则

支持把以下 STM32 示例文本转换为现有 parser 可接受的格式：

- `GESTURE|RIGHT_OPEN|0.86`
  - 转为 `GESTURE:ID=RIGHT_OPEN,CONF=86,HOLD=0`
- `BRINGUP|OK`
  - 转为 `BRINGUP:STATUS=OK`
- `CTRL|LIGHT_ON`
  - 转为 `CTRL:DEV=LIGHT,ACT=ON`
- `CTRL|LIGHT|ON`
  - 转为 `CTRL:DEV=LIGHT,ACT=ON`

如果串口直接输出的已经是：

- `BRINGUP:...`
- `GESTURE:...`
- `CTRL:...`

则会原样透传给现有 `framePipeline -> frameParser`。

### 3. server.js 已接入真实串口分支

已读取环境变量：

- `SERIAL_PORT`
- `SERIAL_BAUDRATE`

同时保留了旧变量兼容：

- `SERIAL_BAUD`

### 4. 输入优先级已调整

当前 `startInputSource()` 的优先级为：

```text
serial > replay > mock
```

逻辑：

- `MODE=serial` 时优先尝试真实串口
- 串口初始化失败时：
  - 若 `USE_REPLAY=true`，则自动回退到 replay
  - 否则回退到 mock

## 本轮改动文件

### 已修改

- `serial-mqtt-ai-bridge/src/server.js`
- `serial-mqtt-ai-bridge/.env.example`
- `serial-mqtt-ai-bridge/package.json`

### 已新增

- `serial-mqtt-ai-bridge/src/serialInput.js`

## 验证结果

### 1. 语法检查通过

已通过：

- `src/serialInput.js`
- `src/server.js`

### 2. mock 模式验证通过

已确认：

- mock 仍可启动
- `/health` 可访问
- `/api/parse-frame` 可正常工作
- task 状态仍正常更新

### 3. replay 模式验证通过

已确认：

- replay 仍可启动
- `replay_engine_task` 可进入 `DONE`

### 4. serial 优先级验证通过

已确认：

- `MODE=serial` 时会先尝试串口
- 当前环境缺少 `serialport` 依赖时，会优雅失败
- 失败后能自动回退到 replay
- 不会破坏原有 demo

## 当前已知情况

当前代码已经支持真实串口入口，但本机这次验证环境里还没有安装 `serialport`，所以真实串口未完成实机读取验证。

这不影响当前改动方向，原因是：

- 串口依赖是懒加载
- 没有串口依赖时不会拖垮 mock / replay
- fallback 行为已验证正常

## 下一步建议

下一步最值得做的是：

1. 在可联网环境执行 `npm install`
2. 确认 `serialport` 依赖安装成功
3. 接上真实 STM32 串口输出
4. 验证 STM32 文本帧是否能进入 `framePipeline -> frameParser -> websocket`

## 本轮未做的事

本轮没有做这些改动：

- 没有修改 `frameParser.js`
- 没有修改 dashboard
- 没有修改 STM32 固件
- 没有删除 mock / replay
- 没有重构 `framePipeline`
- 没有改 WebSocket 旧协议结构
