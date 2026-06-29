# 给 Codex 的硬件背景提示词

> 复制粘贴下面全部内容给 Codex，它就能理解硬件接线和约束。

---

你现在接手一个 STM32F407ZGT6 嵌入式项目："双手手语翻译手套"（v3.1）。以下是硬件工程信息，改代码前请确认引脚分配和电路约束。

## 硬件平台

- **主控**：STM32F407ZGT6，168MHz，1MB Flash，192KB SRAM
- **编译**：Keil MDK-ARM v5，CubeMX 生成 HAL 库框架
- **系统时钟**：HSE 8MHz → PLL 168MHz。APB1 42MHz，APB2 84MHz
- **调度时钟**：TIM3 1ms 中断（PSC=83, ARR=999 → 84MHz/84/1000 = 1kHz）

## 全部引脚分配（改代码前必须核对）

```
=== ADC（10 路手指弯曲传感器，12 位，DMA 循环模式）===
ADC1_IN0  PA0   右手拇指
ADC1_IN1  PA1   右手食指
ADC1_IN2  PA2   右手中指
ADC1_IN3  PA3   右手无名指
ADC1_IN4  PA4   右手小指
ADC2_IN5  PA5   左手拇指
ADC2_IN6  PA6   左手食指
ADC2_IN7  PA7   左手中指
ADC2_IN8  PB0   左手无名指
ADC2_IN9  PB1   左手小指

=== 软 I2C（GPIO 翻转模拟，400kHz，3 路独立）===
I2C_1: PB6(SCL) PB7(SDA)   右手 JY61P IMU (0xA0 写地址)
I2C_2: PB8(SCL) PB9(SDA)   左手 JY61P IMU (0xA0 写地址)
I2C_3: PC6(SCL) PC7(SDA)   MAX30102 心率血氧 (0xAE 写地址)

=== 硬件 UART ===
USART1: PA9(TX) PA10(RX)    DFPlayer Mini 语音模块（DMA 发送，非阻塞）
USART3: PC10(TX) PC11(RX)   JDY-33 蓝牙模块（BLE 4.2, Web Bluetooth 兼容）

=== 软 UART（GPIO 模拟，115200bps，TX only）===
PE0 → ESP-01S RX            DWT 周期计数器精确定时，1458 cycles/bit

=== 定时器 ===
TIM3             1ms 系统 tick（调度器时钟源）
TIM4 CH1 PE2     右手振动马达 PWM
TIM4 CH2 PE3     右手振动马达 PWM
TIM4 CH3 PE4     左手振动马达 PWM
TIM4 CH4 PE5     左手振动马达 PWM
```

## 供电拓扑

```
18650×2 并联 (3.7V, ~5200mAh)
  → TP4056 充电保护
    → MT3608 升压 (5V)
      ├→ AMS1117 #1 (3.3V) → F407 + 传感器 + DFPlayer + JDY-33
      ├→ AMS1117 #2 (3.3V) → MAX30102（LC 滤波隔离音频噪声）
      └→ 5V 直供 → ESP-01S（WiFi 模块需要 5V）
```

## 传感器特性

| 传感器 | 型号 | 接口 | 关键特性 |
|--------|------|------|---------|
| 手指弯曲 ×10 | 电阻式 flex sensor 10KΩ 分压 | ADC (12bit) | 弯曲越大电阻越大，分压越低。软件做 [0,100] 百分比映射 |
| 右手 IMU | 维特智能 JY61P | 软 I2C 400kHz | 内置卡尔曼滤波 200Hz。一次 Burst Read (0x34, 18B) 得 ACC(m/s²)+GYRO(°/s)+ANGLE(°) |
| 左手 IMU | 维特智能 JY61P | 软 I2C 400kHz | 同上 |
| 心率血氧 | MAX30102 | 软 I2C 400kHz | 100Hz FIFO 读取。IR 边界 [5000, 200000]。独立 LC 供电防 DFPlayer 音频干扰 |

## 通信模块

| 模块 | 型号 | 用途 | 注意 |
|------|------|------|------|
| 蓝牙 | **JDY-33** (不是 JDY-31) | 手机 Web Bluetooth → 18B 遥测帧 | JDY-31 是经典蓝牙 SPP，不支持 Web BLE。JDY-33 引脚兼容直接替换 |
| WiFi | ESP-01S (ESP8266) | 手势触发 → AT+MQTTPUB → MQTT 继电器 | 必须预配网（AT+CWJAP 等）。F407 只发 ASCII AT 命令，不直接组 MQTT 包 |

## 代码中不能改的硬件约束（强行改了会炸）

1. **软 I2C 有 2000 次超时熔断** — 防止 SCL 被外设拉死导致死循环。不要删
2. **ADC 用 Ping-Pong 双缓冲**（HalfCplt / Cplt 中断各锁一半）— Flex_Update() 只读稳定半组。不要再加 DMA Stop/Start
3. **MAX30102 FIFO 用 while(unread>0) 一次清空** — 不要改成读 1 个样本就走
4. **软 UART 发送时关总中断 `__disable_irq()`** — 保证 GPIO 位时序不被硬件中断打断。发送完必须 `__enable_irq()`。单次阻塞 < 4ms @ 115200bps
5. **TIM3 必须 HAL_TIM_Base_Start_IT(&htim3)** — 1ms 中断是全部调度器的时钟源，忘了启动整个系统死锁
6. **JY61P 需在 PC 上位机切为 I2C 模式**（出厂默认串口）— 只切一次固化保存。如果代码跑不起来先检查这个
7. **MAX30102 和 DFPlayer 不能共用同一路 3.3V** — 音频功放瞬态电流会干扰 PPG 信号。硬件上已做独立 LC 供电
8. **USART1 被 DFPlayer 占用** — 不要把调试 printf 打到 PA9。调试输出走 USART3 (PC10)
9. **Keil 工程添加 .c 文件必须手动在 IDE 里 Add** — 直接改 .uvprojx XML 会被 Keil IDE 还原
