# 双手手语翻译手套 — Codex 接手指南

> 写于 v3.1，请在开始任何改动前**先完整读完本文档**。

---

## 一、项目概况

这是一个**嵌入式大赛（ST 赛道）参赛项目**：双手穿戴式手语翻译手套。面向失语症患者术后交流 + 老年护理场景。

### 它能做什么

| 模式 | 功能 | 触发方式 |
|------|------|---------|
| **翻译模式** (MODE_TRANSLATE) | 12 个手语手势 → DFPlayer 本地真人语音播报 | 双手 10 指弯曲 + 右手运动方向 |
| **IoT 控制模式** (MODE_CONTROL) | 6 个控制手势 → ESP-01S WiFi 模块 MQTT 控制灯/风扇/插座 | 同上，查 ctrl_vocab 表 |
| **AI 康复模式** (MODE_REHAB) | 5 步渐进训练（握拳→伸展→拇指弯→OK→比耶），Web 端 DeepSeek AI 教练 | 同上，手机 Chrome Web App |

三种模式通过**双手全握 3 秒**循环切换（切换时 DFPlayer 语音提示）。

### 硬件清单

| 模块 | 型号 | 数量 | 接口 |
|------|------|:---:|------|
| 主控 | STM32F407ZGT6 (168MHz, 1MB Flash) | 1 | — |
| IMU | 维特智能 JY61P（卡尔曼滤波, 200Hz, I2C） | 2 | 软 I2C (PB6-7 / PB8-9) |
| 柔性传感器 | 电阻式 10K 分压 | 10 | ADC1 (PA0-4) + ADC2 (PA5-7, PB0-1) |
| 心率血氧 | MAX30102 | 1 | 软 I2C (PC6-7) |
| 语音播报 | DFPlayer Mini | 1 | USART1 (PA9 TX DMA) |
| 蓝牙 | JDY-33 (⚠️ 必须是 JDY-33 不是 JDY-31，要 BLE 兼容 Web Bluetooth) | 1 | USART3 (PC10 TX, PC11 RX) |
| WiFi | ESP-01S (ESP8266, AT 固件) | 1 | PE0 软 UART (TX only) |
| 振动反馈 | 扁平马达 ×2 | 2 | TIM4 PWM (PE2-3, PE4-5) |

---

## 二、当前版本: v3.1

### 版本历程

```
v2.2  基础框架 (手势识别 + DFPlayer + 蓝牙)
v2.3  7 大防御性重构 (软 I2C 超时熔断, 旋转熔断, ADC DMA 原子快照...)
v2.4  4 项暗桩清除 (JY61P 替换 MPU6050, MAX30102 FIFO 清仓, ADC Ping-Pong, 250ms 松弛锁存窗)
v3.0  IoT 控制 + AI 康复 + 3 个 Web 页面
v3.1  4 个致命盲点修复 (JDY-31→JDY-33, 软 UART 115200 AT 命令, 手势冻结锁, DFPlayer 模式切换清空)
```

### v3.1 的 4 个关键修复（给 Codex 的背景）

1. **JDY-31 不兼容 Web Bluetooth** → 换 JDY-33（硬件，用户需要买），引脚兼容
2. **ESP-01S AT 固件不能解析二进制 MQTT** → 改用 ASCII 格式 `AT+MQTTPUB=0,"topic","payload",0,0\r\n`
3. **9600bps 软 UART 阻塞 15ms+ 打穿 5ms 调度槽** → 升到 115200bps (DWT 1458 cycles/bit)，`__disable_irq()` 全程保护，阻塞 < 4ms
4. **双手全握 Hold 3 秒期间语音寄生触发** → `Gesture_Freeze()` / `Gesture_Unfreeze()` 冻结锁，`DFPlayer_Stop()` 清 DMA 残留

---

## 三、项目文件结构

```
双手中文手语翻译手套/
├── Core/
│   ├── Inc/                          # 头文件
│   │   ├── main.h
│   │   ├── soft_i2c.h               # 三路软 I2C（400kHz, 2000 次超时熔断）
│   │   ├── jy61p.h                  # JY61P I2C Burst Read (ACC+GYRO+ANGLE 12+6B)
│   │   ├── flex_sensor.h            # 10 路柔性传感器 (Ping-Pong 双缓冲 ADC)
│   │   ├── gesture.h                # 手势识别引擎 (五态状态机 + 模式管理)
│   │   ├── max30102.h               # MAX30102 心率血氧 (FIFO 清仓 + 边界卡关)
│   │   ├── dfplayer.h               # DFPlayer Mini (DMA 非阻塞发送)
│   │   ├── bluetooth.h              # JDY-33 蓝牙 (环形缓冲 + BT_SendRaw)
│   │   ├── vibrator.h               # 双路振动马达 (TIM4 PWM)
│   │   ├── soft_uart.h              # ★ PE0 软 UART TX (115200bps AT+MQTTPUB)
│   │   └── wifi.h                   # (占位, 未使用)
│   ├── Src/
│   │   ├── main.c                   # ★ 主调度器 (6 级多速率时序)
│   │   ├── soft_i2c.c
│   │   ├── jy61p.c
│   │   ├── flex_sensor.c
│   │   ├── gesture.c                # ★ 手势识别核心 (五态状态机)
│   │   ├── max30102.c
│   │   ├── dfplayer.c
│   │   ├── bluetooth.c
│   │   ├── vibrator.c
│   │   ├── soft_uart.c              # ★ 软件 UART (PE0 TX, DWT 精确定时)
│   │   ├── adc.c                    # Ping-Pong HalfCplt/Cplt 中断
│   │   ├── dma.c
│   │   ├── tim.c
│   │   ├── usart.c
│   │   └── gpio.c
│   └── Startup/
│       └── startup_stm32f407xx.s
├── web/                              # ★ 3 个纯 HTML 单页 Web App
│   ├── dashboard.html               # 实时仪表盘 (Web Bluetooth + Canvas)
│   ├── control.html                 # IoT 设备手动控制 (MQTT 配置)
│   └── rehab.html                   # AI 康复训练 (DeepSeek + Web Speech API)
├── Drivers/                          # CubeMX 生成的 HAL 库
├── MDK-ARM/
│   └── shuangshou/                   # Keil 工程 (.uvprojx)
└── .vscode/                          # VS Code 配置
```

---

## 四、核心架构

### 4.1 多速率时序调度（6 级，TIM3 1ms 系统时钟）

```
┌──────────┬─────────┬──────────────────────────────────────────┐
│ 周期     │ 频率    │ 任务                                      │
├──────────┼─────────┼──────────────────────────────────────────┤
│  5ms     │ 200Hz   │ JY61P 双路 Burst Read (ACC+GYRO+ANGLE)   │
│ 10ms     │ 100Hz   │ 三步跌倒检测 + MAX30102 FIFO 分时读取      │
│ 20ms     │  50Hz   │ Ping-Pong ADC 快照 + Flex_Update + 痉挛检测│
│ 50ms     │  20Hz   │ 手势识别 + DFPlayer 触发 + 振动维护        │
│ 100ms    │  10Hz   │ 模式切换检测 + 蓝牙消费 + PPG 批量处理     │
│ 200ms    │   5Hz   │ 18B 蓝牙遥测帧 (手指+姿态+HR/SpO2)        │
└──────────┴─────────┴──────────────────────────────────────────┘
```

**调度器设计原则**：
- 所有时间戳比较基于 `sys_tick_ms`（TIM3 1ms 中断递增）
- 每个 slot 内没有阻塞等待（除了软 UART 发送有 ~3.9ms 关中断窗口）
- JY61P 单次 Burst Read ~560μs @ 400kHz，占 5ms slot 的 22.4%

### 4.2 手势识别流程（五态状态机）

```
ST_WAIT_STABLE ──(手形保持 500ms)──→ ST_WAIT_TRAJ
ST_WAIT_TRAJ ────(检测到方向/1500ms超时)─→ ST_ARMED
ST_ARMED ────────(组装 11 位编码)──→ ST_LATCH       ← 250ms 松弛锁存窗 (v2.4)
ST_LATCH ───────(窗口关闭, 查表触发)─→ ST_COOLDOWN
ST_COOLDOWN ────(800ms)───────────→ ST_WAIT_STABLE
```

**11 位编码**: 10 位手指三态 (0=伸直/1=半弯/2=全弯) + 1 位方向 (0-6)

**防御性保护**:
- 旋转熔断: 陀螺角速度平方和 > 3600 (°/s)² → 方向判定静默 200ms
- 冷启动保护: 前 20 个采样静默
- 百分比硬钳位: [0, 100]
- 手势冻结: v3.1 新增，模式切换 Hold 期间阻止所有输出

### 4.3 IoT 控制链路

```
手势识别 → ctrl_vocab[] 查表
         → SoftUART_SendMQTT(topic, payload)
         → snprintf("AT+MQTTPUB=0,\"%s\",\"%s\",0,0\r\n", topic, payload)
         → PE0 GPIO 翻转 @ 115200bps (DWT 精确定时)
         → ESP-01S AT 固件自动发布 MQTT
         → 云端 broker (broker.emqx.io:1883)
         → 继电器端订阅
```

**控制词表** (ctrl_vocab[] in gesture.c):
| 编码 | Topic | Payload |
|--------|---------------|---------|
| 00000110000 | glove/light | ON |
| 00000110001 | glove/light | OFF |
| 00000220000 | glove/fan | ON |
| 00000220001 | glove/fan | OFF |
| 00000120000 | glove/socket | ON |
| 00000120001 | glove/socket | OFF |

### 4.4 蓝牙遥测帧格式（v3.0 18 字节二进制，200ms 周期）

```
[0xAA] [R5指×5] [Roll_L Roll_H Pitch_L Pitch_H Yaw_L Yaw_H] [HR] [SpO2] [Mode] [L5指×5] [XOR] [0xBB]
```

### 4.5 引脚映射（关键！勿改）

```
ADC:
  ADC1_IN0~4  PA0~PA4    右手 5 指弯曲
  ADC2_IN5~7  PA5~PA7    左手 3 指
  ADC2_IN8~9  PB0~PB1    左手 2 指

软 I2C (GPIO 翻转, 400kHz):
  SoftI2C_1   PB6(SCL) PB7(SDA)   右手 JY61P (0xA0 写地址/0xA1 读地址)
  SoftI2C_2   PB8(SCL) PB9(SDA)   左手 JY61P
  SoftI2C_3   PC6(SCL) PC7(SDA)   MAX30102

UART:
  USART1      PA9(TX)              DFPlayer Mini (DMA 非阻塞)
  USART3      PC10(TX) PC11(RX)    JDY-33 蓝牙

软 UART:
  PE0         TX only              接 ESP-01S RX (115200bps)

TIM:
  TIM3                            1ms 系统调度时钟 (PSC=83, ARR=999)
  TIM4 CH1-2 PD12-13             右手/左手振动 PWM (各1路, 共2路)
  ⚠️ 注意: 文档曾写 PE2-PE5 四路, 实际 .ioc 和代码是 PD12-PD13 两路
```

---

## 五、关键设计约束（不要破坏！）

### 5.1 安全约束

1. **软 I2C 超时熔断**: `soft_i2c.c` 中每个 SCL 等待循环最多 2000 次（~50μs），超过直接 return 1。**不要删掉这个超时保护。**
2. **MAX30102 FIFO 清仓**: `MAX30102_ReadFIFO()` 用 `while(unread > 0)` 一次性读空 FIFO。不要改成只读 1 样本。
3. **ADC Ping-Pong 双缓冲**: `adc.c` 的 `HAL_ADC_ConvHalfCpltCallback` 和 `HAL_ADC_ConvCpltCallback` 分别标记前后半组完成。`Flex_Update()` 只读稳定的半组。**不要恢复旧版的 DMA Stop/Start 做法。**
4. **软 UART `__disable_irq()`**: 发送全程关总中断保护 GPIO 波形，**发送完后必须 `__enable_irq()`**。最多阻塞 ~3.9ms。
5. **手势冻结锁**: 调用 `Gesture_Freeze()` 后**必须配对** `Gesture_Unfreeze()`，否则手势识别永不复活。

### 5.2 编译约束

- **编译工具链**: Keil MDK-ARM v5 (ARM Compiler 5/6)
- **CubeMX 生成的文件不要手动改**（`main.h`, `adc.c`, `tim.c`, `usart.c`, `gpio.c`, `dma.c` 中非 USER CODE 区域）
- **新增 `.c` 文件必须手动在 Keil IDE 中 Add to Project**（Keil 的 `.uvprojx` XML 脚本修改会被 Keil IDE 还原）
- **编译目标**: 0 Error, 0 Warning

### 5.3 时序约束

- 5ms 槽内 JY61P 读取 ~1120μs，剩余 3880μs 给其他逻辑。**不要在 5ms 任务加阻塞操作。**
- 软 UART 发送 ~3.9ms，发生在 50ms 任务中（手势触发时）。假设 5ms/10ms/20ms 任务可能被推迟一个周期。
- **绝对不要用 `HAL_Delay()`**，它会阻塞整个调度器。

---

## 六、Web 页面（手机 Chrome 打开）

三个独立 HTML 文件，无需服务器，直接用 `file://` 或 Live Server 打开：

| 文件 | 用途 | 关键依赖 |
|------|------|---------|
| `web/dashboard.html` | BLE 实时仪表盘（手指条形图 + 姿态球 + HR/SpO2） | Web Bluetooth API (需要 JDY-33) |
| `web/control.html` | 手动控制灯/风扇/插座（MQTT 配置存 localStorage） | MQTT broker |
| `web/rehab.html` | 5 步康复训练 + DeepSeek AI 教练语音对话 | ⚠️ 需要用户填 DeepSeek API Key |

### rehab.html 注意

- 第 1 行附近有 `const API_KEY='YOUR_DEEPSEEK_API_KEY';` — 用户需要替换成真实 Key
- 使用 Web Speech API（语音识别 + 合成）
- DeepSeek API endpoint: `https://api.deepseek.com/chat/completions`

---

## 七、ESP-01S 前置配置（一次性）

ESP-01S 需要先用 USB-TTL 刷 AT 固件，然后手动发以下 AT 命令配网：

```
AT+CWMODE=1
AT+CWJAP="你的WiFi名","WiFi密码"
AT+MQTTUSERCFG=0,1,"","","",0,0,""
AT+MQTTCONN=0,"broker.emqx.io",1883,0
```

配好后，F407 通过软 UART 发的 `AT+MQTTPUB` 命令 ESP-01S 会自动执行。

---

## 八、Git 仓库信息

```
仓库: github.com/你的用户名/仓库名
当前 HEAD: dd503b3 (v3.1)
分支: main
```

最近提交:
```
dd503b3 fix: v3.1 — 4 fatal blind spots from Gemini review
d02d748 feat: v3.0 IoT control + AI rehab + 3 web pages
fd3fe93 feat: v2.4 industrial defense — 4 hidden bomb fixes
7163a6e feat: MPU6050 -> JY61P I2C drop-in
6d59bf6 feat: defensive refactor v2.3 - fix 7 hardware bombs
ba87506 feat: 双手手语翻译手套 v2.2 软件框架
```

---

## 九、已知待办事项

1. **硬件采购**: 用户需要买 JDY-33 替换 JDY-31（Web Bluetooth 兼容）
2. **DeepSeek API Key**: 用户在 `web/rehab.html` 中填入自己的 Key
3. **ESP-01S 配网**: 用户需要按第七节步骤预配置
4. **Keil 工程**: 用户需要手动加 `soft_uart.c` 到 Keil 项目组
5. **实际硬件测试**: v3.1 代码尚未在真实 F407 板上验证（用户假期不在实验室）
6. **JY61P I2C 模式**: 需要在 PC 上位机软件中把 JY61P 从默认串口模式切到 I2C 模式（只需切一次）

---

## 十、相关文档

| 文档 | 路径 |
|------|------|
| 完整设计方案 (v2.4) | `E:\DATA\123\HT32F52352_LQFP64\双手中文手语翻译手套-设计方案.md` |
| v3.0 设计说明 | `E:\DATA\123\HT32F52352_LQFP64\v3.0-IoT控制-AI康复-设计方案.md` |
| v3.0 实施计划 | `E:\DATA\123\HT32F52352_LQFP64\v3.0-实施计划.md` |
| 硬件采购清单 | `E:\DATA\123\HT32F52352_LQFP64\硬件采购清单_v2.md` |

---

## 十一、给 Codex 的工作建议

1. **改动任何 .c/.h 之前先读对应的头文件**，理解完整的 API 契约
2. **改 main.c 时格外小心** —— 6 级时序耦合性强，加新任务要评估槽容量
3. **手势识别是核心敏感区** —— 改 gesture.c 前先理解五态状态机状态迁移图
4. **测试时候先确认 TIM3 1ms 中断在跑**（看 `sys_tick_ms` 是否递增）
5. **调试输出走 USART3 (PC10)** 不是 USART1 (PA9, 被 DFPlayer 占用)
6. **编译前确认所有 `.c` 文件已在 Keil 工程中**（最常见的编译错误原因）
7. **不要假设硬件都在线** —— JY61P、MAX30102 都可能不在线，代码已做降级处理

---

## 十二、最近变更记录 (2026-06-29)

### 注释清理
- **main.c**: 修复了 ~110 行 UTF-8 乱码注释（调度表、防御设计、阶段初始化、内联注释、中断回调文档），所有中文注释恢复正常。PUA 字符已清零。
- **gesture.c**: 修正 "车架师"→"架构师"，版本号 v2.4→v3.1，中文全部正常。
- **注意**: 只改了注释，一行代码逻辑都没动。

### 硬件映射差异表
- 新文件: `docs/设计目标-vs-代码实现-差异表.md`
- 对照 `.ioc`、代码实际、设计文档、`current-pin-matrix.md` 四份来源
- 关键发现:
  - 🔴 **振动 PWM 引脚**: 代码 `PD12-PD13` (2ch) ≠ 文档 `PE2-PE5` (4ch)
  - 🔴 **蓝牙**: 必须用 JDY-33 BLE，当前 JDY-31 不兼容 Web Bluetooth
  - ⚠️ **死代码**: `mpu6050.c/h` 仍在项目中（已迁移到 jy61p.c/h）
  - ⚠️ **注释过时**: `soft_i2c.c/h` 仍写 MPU6050 @ 100kHz，实际 JY61P @ 400kHz
  - ⚠️ **PE0 软串口**: `.ioc` 未体现，`soft_uart.c` 手动初始化
  - ⚠️ **时钟混用**: `gesture.c` 用 `HAL_GetTick()`，调度器用 `sys_tick_ms`

### 新增文档
- `docs/设计目标-vs-代码实现-差异表.md` — 完整的已一致/不一致/待确认三栏对照表
- `硬件现状清单-v3.1.md` — 当前硬件状态（含采购待办）
- `codex-hardware-prompt.md` — 给 Codex 的硬件专用提示词

---

## 十三、相关文档（更新）

| 文档 | 路径 |
|------|------|
| 完整设计方案 (v2.4) | `E:\DATA\123\HT32F52352_LQFP64\双手中文手语翻译手套-设计方案.md` |
| v3.0 设计说明 | `E:\DATA\123\HT32F52352_LQFP64\v3.0-IoT控制-AI康复-设计方案.md` |
| v3.0 实施计划 | `E:\DATA\123\HT32F52352_LQFP64\v3.0-实施计划.md` |
| 硬件采购清单 | `E:\DATA\123\HT32F52352_LQFP64\硬件采购清单_v2.md` |
| ★ 硬件现状清单 | `硬件现状清单-v3.1.md` |
| ★ 设计 vs 代码差异表 | `docs/设计目标-vs-代码实现-差异表.md` |
| ★ 当前代码引脚表 | `docs/current-pin-matrix.md` |
| ★ 无硬件联调清单 | `docs/no-hardware-bringup-checklist.md` |
| ★ Codex 硬件提示词 | `codex-hardware-prompt.md` |
