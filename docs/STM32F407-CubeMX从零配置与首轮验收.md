# STM32F407 从零配置与首轮验收

本文用于重新建立一个干净的 STM32F407ZGT6 基础工程。第一阶段只锁定三条最重要的链路：

1. JDY-33 蓝牙串口
2. 10 路 FLEX 应变片 ADC 采集
3. 一块 JY61P 硬件 I2C 角度采集

另外预先配置一个 PWM 蜂鸣器，但暂时不把 MAX30102、第二块 JY61P、DFPlayer、ESP-01S 和复杂手势逻辑加入基础工程。

## 1. 最终引脚表

| 功能 | STM32 引脚 | CubeMX 配置 | 说明 |
|---|---|---|---|
| 右手 FLEX 1~5 | PA0~PA4 | ADC1_IN0~IN4 | 模拟输入，无上下拉 |
| 左手 FLEX 1~5 | PA5~PA7、PB0、PB1 | ADC2_IN5~IN9 | 模拟输入，无上下拉 |
| JY61P SCL | PB6 | I2C1_SCL | 硬件 I2C，100 kHz |
| JY61P SDA | PB7 | I2C1_SDA | 外接上拉到 3.3 V |
| JDY-33 TXD | PB10 | USART3_TX | 接 JDY-33 RXD |
| JDY-33 RXD | PB11 | USART3_RX | 接 JDY-33 TXD |
| 蜂鸣器 PWM | PD14 | TIM4_CH3 | 低电平触发模块，初始 2 kHz/50% |
| 调试下载 | PA13、PA14 | SYS Serial Wire | ST-Link SWD |

### 重要说明

- 本阶段只接一块 JY61P，默认 I2C 7 位地址为 `0x50`。
- HAL 调用这个地址时使用左移后的 `0xA0`，不要把 `0x50` 直接传给 HAL。
- JY61P 必须提前用维特上位机切到 I2C 模式并保存；仍处于串口模式时，PB6/PB7 上不会有正确的寄存器响应。
- I2C 总线必须有 SDA、SCL 上拉。优先使用模块自带上拉；没有时给 SDA、SCL 各接一个 4.7 kOhm 到 3.3 V。
- 所有模块必须共地。JY61P、JDY-33 和 STM32 的信号电压按 3.3 V 连接。
- 低电平触发的无源蜂鸣器模块仍需要 PWM 波形；如果手里是裸蜂鸣器，不能直接由 STM32 引脚带动，应使用三极管或 MOS 管驱动。

## 2. 新建 CubeMX 工程

1. 打开 STM32CubeMX。
2. 点击 `File` -> `New Project`。
3. 选择 `MCU/MPU Selector`。
4. 搜索并选择 `STM32F407ZGT6`，确认封装为 `LQFP144`。
5. 点击 `Start Project`。
6. 不要直接覆盖现有旧工程。建议工程名使用 `shuangshou_base_f407`，放到单独目录，例如：

   `E:\DATA\STM32\cubemx\shuangshou_base_f407`

7. 进入 `Project Manager`：
   - `Project Name`：`shuangshou_base_f407`
   - `Project Location`：上面的独立目录
   - `Toolchain / IDE`：`MDK-ARM`
   - `Code Generator` 中勾选 `Generate peripheral initialization as a pair of '.c/.h' files per peripheral`
   - 如果有 `Keep User Code when re-generating`，保持勾选

## 3. 先配置系统基础项

### 3.1 SYS

1. 打开 `Pinout & Configuration`。
2. 找到 `System Core` -> `SYS`。
3. `Debug` 选择 `Serial Wire`。
4. `Timebase Source` 保持 `SysTick`。

这样会保留 PA13/PA14 给 ST-Link 下载和调试。

### 3.2 RCC

1. 找到 `System Core` -> `RCC`。
2. 如果开发板有外部 8 MHz 晶振，把 `High Speed Clock (HSE)` 设置为 `Crystal/Ceramic Resonator`。
3. 没有外部晶振时可以暂时使用 HSI，但本项目推荐使用稳定的 8 MHz HSE。

### 3.3 时钟

进入 `Clock Configuration`，确认最终结果：

| 参数 | 值 |
|---|---:|
| SYSCLK | 168 MHz |
| HCLK | 168 MHz |
| APB1 | 42 MHz |
| APB2 | 84 MHz |
| APB1 Timer Clock | 84 MHz |
| APB2 Timer Clock | 168 MHz |

如果使用 8 MHz HSE，常用 PLL 参数为：

- PLLM = 8
- PLLN = 336
- PLLP = 2
- PLLQ = 7
- APB1 Prescaler = `/4`
- APB2 Prescaler = `/2`

点击页面上的校验按钮，确保没有红色错误。

## 4. 配置 JDY-33 蓝牙

1. 回到 `Pinout & Configuration`。
2. 找到 `Connectivity` -> `USART3`。
3. 选择 `Asynchronous`。
4. 检查引脚自动分配为：
   - `PB10` -> `USART3_TX`
   - `PB11` -> `USART3_RX`
5. 点击 `USART3` 参数设置：
   - Baud Rate：`9600 Bits/s`
   - Word Length：`8 Bits`
   - Parity：`None`
   - Stop Bits：`1`
   - Data Direction：`Receive and Transmit`
   - Hardware Flow Control：`Disable`
   - Oversampling：`16`
6. 在 `NVIC Settings` 中打开 `USART3 global interrupt`，后续如果使用中断接收会用到。

### 蓝牙接线

| JDY-33 | STM32 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| TXD | PB11 / USART3_RX |
| RXD | PB10 / USART3_TX |

注意 TX 和 RX 必须交叉连接。第一阶段不使用 AT 配置，只验证 STM32 能持续发送文本。

## 5. 配置右手 5 路 ADC

### 5.1 ADC1 通道

在 Pinout 页面依次点击：

- PA0 -> `ADC1_IN0`
- PA1 -> `ADC1_IN1`
- PA2 -> `ADC1_IN2`
- PA3 -> `ADC1_IN3`
- PA4 -> `ADC1_IN4`

打开 `Analog` -> `ADC1`，设置：

- Resolution：`12 Bits`
- Scan Conversion Mode：`Enabled`
- Continuous Conversion Mode：`Enabled`
- Discontinuous Mode：`Disabled`
- Number Of Conversion：`5`
- Data Alignment：`Right Alignment`
- External Trigger：`Software Start`
- DMA Continuous Requests：`Enabled`
- EOC Selection：`End of each conversion`
- Clock Prescaler：`PCLK2/4`

在 `Rank` 中严格设置：

| Rank | Channel |
|---:|---|
| 1 | IN0 / PA0 |
| 2 | IN1 / PA1 |
| 3 | IN2 / PA2 |
| 4 | IN3 / PA3 |
| 5 | IN4 / PA4 |

每个通道的 Sampling Time 建议先统一为 `480 Cycles`。应变片分压源阻抗较高，采样时间太短会导致读数不稳定。

### 5.2 ADC2 通道

依次点击：

- PA5 -> `ADC2_IN5`
- PA6 -> `ADC2_IN6`
- PA7 -> `ADC2_IN7`
- PB0 -> `ADC2_IN8`
- PB1 -> `ADC2_IN9`

打开 `ADC2`，设置与 ADC1 完全相同：

- Scan Enabled
- Continuous Enabled
- Number Of Conversion = `5`
- DMA Continuous Requests = `Enabled`
- Software Start
- PCLK2/4
- 每个通道 `480 Cycles`

Rank 顺序必须是 `IN5、IN6、IN7、IN8、IN9`。这个顺序后续直接对应左手 5 根手指，不能在代码里随便交换。

### 5.3 配置 DMA

进入 ADC1 的 `DMA Settings`，点击 `Add`：

- Direction：`Peripheral to Memory`
- Mode：`Circular`
- Peripheral Data Alignment：`Half Word`
- Memory Data Alignment：`Half Word`
- Peripheral Increment：`Disable`
- Memory Increment：`Enable`
- Priority：`Low` 或 `Medium`
- FIFO：`Disable`

ADC2 也添加同样的 DMA 配置。让 CubeMX 自动选择合法的 DMA Stream/Channel，不要手工照搬旧工程的 Stream。

在 `NVIC Settings` 中打开 ADC1、ADC2 对应 DMA Stream 的中断。后续代码会用半传输和全传输回调读取稳定快照。

### ADC 接线限制

- 应变片分压输出必须在 `0~3.3 V` 内。
- 不要把任何 ADC 引脚接 5 V。
- ADC 引脚配置为 `Analog`，不要配置为 GPIO 推挽、上拉或下拉。
- 没接传感器的通道会漂浮，首轮只看已接通道，不能用浮空通道判断 ADC 是否损坏。

## 6. 配置一块 JY61P 的硬件 I2C

### 6.1 启用 I2C1

1. 在 Pinout 页面找到 `Connectivity` -> `I2C1`。
2. 选择 `I2C`。
3. 确认引脚为：
   - PB6 -> `I2C1_SCL`
   - PB7 -> `I2C1_SDA`
4. 打开 I2C1 参数：
   - I2C Speed：`100 kHz`
   - Addressing Mode：`7-bit`
   - Own Address 1：`0`
   - Dual Address：`Disabled`
   - General Call：`Disabled`
   - No Stretch：`Disabled`

CubeMX 应将 PB6/PB7 生成为复用开漏输出。不要再对 PB6/PB7 调用 GPIO 输出高低电平，也不要把这两个脚留给旧的 `soft_i2c.c`。

### 6.2 JY61P 接线

| JY61P | STM32 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SCL | PB6 / I2C1_SCL |
| SDA | PB7 / I2C1_SDA |

JY61P 端必须完成：

1. 使用维特上位机连接模块。
2. 将模块从串口模式切换为 I2C 模式。
3. 保存配置并重新上电。
4. 确认 7 位地址是 `0x50`。

如果模块仍然输出串口二进制帧，说明它还没有切到 I2C 模式，CubeMX 和接线都正确时也会读不到。

## 7. 配置低电平触发 PWM 蜂鸣器

### 7.1 TIM4_CH3

1. 在 Pinout 页面找到 `Timers` -> `TIM4`。
2. 选择 `PWM Generation CH3`。
3. 将引脚确认设为 `PD14 / TIM4_CH3`。
4. 进入 TIM4 参数设置：
   - Prescaler：`83`
   - Counter Period：`499`
   - Counter Mode：`Up`
   - Clock Division：`DIV1`
   - Auto-reload preload：`Enable` 或保持默认
5. 进入 CH3 的 PWM 设置：
   - Pulse：`250`
   - Fast Mode：`Disable`
   - Output Compare Preload：`Enable`
   - Polarity：先按低电平触发模块选择 `Low`

计算依据：TIM4 时钟约为 84 MHz，`84 MHz / (83 + 1) = 1 MHz`，再用 `ARR=499` 得到约 `2 kHz`，`Pulse=250` 得到约 50% 占空比。

### 7.2 蜂鸣器默认状态

生成代码后，先不要自动启动 PWM。应用代码后续使用：

```c
HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3);
```

报警结束时停止：

```c
HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_3);
```

如果实测高低逻辑反了，只修改 `TIM4_CH3` 的 Polarity 或驱动模块输入，不改变 PB6/PB7、PB10/PB11、ADC 引脚。

## 8. 生成代码前最后检查

在 CubeMX 的 Pinout 页面确认：

- PA0~PA4 全部为 ADC1 模拟输入
- PA5~PA7、PB0、PB1 全部为 ADC2 模拟输入
- PB6/PB7 只有 I2C1，没有 GPIO Output
- PB10/PB11 只有 USART3
- PD14 只有 TIM4_CH3
- PA13/PA14 是 Serial Wire
- 没有红色冲突提示

点击 `Project Manager` -> `Generate Code`。

生成后先用 Keil 打开工程，只做一次空工程编译。这个阶段不粘贴旧版 `soft_i2c.c`、旧版 `main.c`、旧版 `jy61p.c`，避免把旧配置重新带回来。

## 9. 生成后应该存在的关键对象

编译前检查这些对象：

- `hadc1`
- `hadc2`
- `hi2c1`
- `huart3`
- `htim4`
- `MX_ADC1_Init()`
- `MX_ADC2_Init()`
- `MX_I2C1_Init()`
- `MX_USART3_UART_Init()`
- `MX_TIM4_Init()`

还要检查 `stm32f4xx_hal_msp.c`：

- USART3 使用 PB10/PB11
- I2C1 使用 PB6/PB7
- ADC GPIO 使用 Analog 模式
- TIM4_CH3 使用 PD14
- ADC DMA 已经链接到 ADC 句柄

## 10. 三阶段硬件验收

### 阶段 A：只测蓝牙

接线：JDY-33 VCC、GND、TXD、RXD。烧录后先让 STM32 每秒发送：

```text
BOOT:STM32F407
```

电脑或手机蓝牙工具收到连续文本，才进入下一阶段。如果收不到，先查 TX/RX 是否交叉、波特率是否为 9600、JDY-33 是否真的连接成功。

### 阶段 B：加入 ADC

只接一根应变片分压到 PA0，其他通道可以不接。观察发送值：

- 手指伸直和弯曲时 PA0 数值应有明显变化
- 变化方向可以后续映射，先不在硬件层强行改成 0~100
- PA0 有变化后，再逐根接 PA1、PA2、PA3、PA4、PA5、PA6、PA7、PB0、PB1

推荐发送格式：

```text
FLEX|L1=0|L2=0|L3=0|L4=0|L5=0|R1=85|R2=0|R3=0|R4=0|R5=0
```

### 阶段 C：加入 JY61P

1. 先断电。
2. 确认 JY61P 已切换为 I2C 模式。
3. 接 PB6=SCL、PB7=SDA、3.3 V 和 GND。
4. 上电后先只测试 `HAL_I2C_IsDeviceReady(&hi2c1, 0xA0, 2, 20)`。
5. 返回 `HAL_OK` 后再读角度寄存器。

推荐发送格式：

```text
IMU|R=0.00|P=0.00|Y=0.00
```

第一阶段只要求三角度值会随模块转动变化。零漂处理应放在应用层：上电静止取一段平均值作为零点，后续输出减去零点；不要在 CubeMX 配置阶段处理。

## 11. 后续接入顺序

基础工程通过后，把生成的整个新工程压缩或复制出来，至少保留：

- `.ioc`
- `Core/Inc`
- `Core/Src`
- `MDK-ARM`

然后再按下面顺序接入：

1. 我先接蓝牙固定帧发送。
2. 再接 ADC 双 DMA 和 10 指映射。
3. 再接 JY61P I2C 读取、零点校准和角度帧。
4. 再接网页/小程序数据协议。
5. 最后接手势库、翻译、康复、MQTT 远控和 SOS。

手指分档建议修正为没有空档的范围：

- `0~35`：伸直，绿色
- `36~75`：半弯，黄色
- `76~100`：全弯，红色

原先的 `36~70` 与 `76~100` 中间缺少 `71~75`，网页识别时必须补齐。

## 12. 暂时不要做的配置

在三条基线没有分别通过前，不要加入：

- MAX30102 的 I2C3
- 第二块 JY61P
- DFPlayer 和音频 DMA
- ESP-01S 和 MQTT
- 动态手势、零漂算法和 SOS 抽搐判定
- 微信小程序复杂页面

这样做的目的不是放弃功能，而是先让每一条硬件链路都有明确的故障边界。后续每加一个模块，都能知道是新增模块的问题，还是基础链路的问题。
