# STM32 与 JDY-23 文本协议

USART3 使用 9600、8N1。每一帧以 `\r\n` 结束。

## Level 2 开发测试命令

这些命令只用于开发测试，统一使用 `TEST:` 前缀。固件上电始终为 `REAL`；测试时必须断开 JDY-23，由 USB-TTL 独占 USART3。测试数据是已经标准化的输入，不再经过手机端 FLEX Zero/Full 校准。

```text
TEST:ENTER
TEST:FLEX|L1=0|L2=10|L3=20|L4=30|L5=40|R1=50|R2=60|R3=70|R4=80|R5=100
TEST:IMU|R=10.00|P=-5.00|Y=2.00
TEST:ACC|X=0.00|Y=0.00|Z=1.00|VALID=1
TEST:APPLY
TEST:EXIT
```

- FLEX 必须一次提供完整十指，且每项为 `0~100`。
- Roll/Pitch/Yaw 必须是有限数，范围 `-180~180` 度。
- ACC 必须是有限数，范围 `-16~16 g`；`VALID` 只能为 `0/1`。
- `APPLY` 只有在本轮 FLEX、IMU、ACC 均合法时才原子提交；非法命令不会覆盖上一份已应用数据。
- VIRTUAL 模式连续 5 秒没有合法测试活动会自动返回 REAL。
- 固件对每条测试命令返回一条限流确认，例如 `[TEST] MODE=VIRTUAL`、`[TEST] APPLY=OK` 或 `[TEST] ERROR=FLEX_INVALID`。
- APPLY 后的结果仍由正式 `FLEX`、`IMU`、`ACC`、报警和手势业务链路产生，不存在测试专用识别或报警逻辑。

## 启动帧

```text
BOOT:STM32F407_BASE
```

## FLEX 帧

```text
FLEX|L1=0|L2=0|L3=0|L4=0|L5=0|R1=0|R2=0|R3=0|R4=0|R5=0
```

- `L1~L5`：按 `flex_sensor.c` 的 `hand=1` 索引读取 ADC1 五个 rank，对应 `PC3(ADC1_IN13), PA1, PA2, PA3, PA4`，当前作为左手。
- `R1~R5`：按 `flex_sensor.c` 的 `hand=0` 索引读取 ADC2 五个 rank，对应 `PA5, PA6, PA7, PB0, PB1`，当前作为右手。
- 数值范围：`0~100`。
- 当前映射：约 0.3V 为 0，约 2.0V 为 100，中间线性映射。

## IMU 帧

```text
IMU|R=0.00|P=0.00|Y=0.00
```

- `R`：Roll。
- `P`：Pitch。
- `Y`：Yaw。
- 数值为相对启动零点的角度，单位为度。
- 当前只做启动平均和约 0.30 度死区，不代表长期漂移补偿已经完成。

## BRINGUP 帧

```text
BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1
```

- `JY=1`：启动阶段 JY61P 探测成功；这是兼容性启动字段，不是运行时在线状态。
- `JY_RET=0`：启动阶段 JY61P 探测成功。
- `JY_RET=1`：I2C 设备未应答。
- `JY_RET=2`：设备可应答，但寄存器读取失败。
- `ADC1/ADC2=1`：对应 DMA 已启动并且已经收到过完整快照。
- `BEEP=1`：工程包含启动蜂鸣器测试，不代表当前正在鸣叫。

固件通过 USART3 普通异步队列在初始化后发送一次，并在运行中周期重发；监护端不得依赖一次启动帧。为兼容现有解析器，前五个字段固定为 `JY,JY_RET,ADC1,ADC2,BEEP`，后续详细字段可忽略：

```text
BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1,MAX=0,MAX_RET=1,DEG=1
```

当前报警输入为右手 JY61P，因此 `JY/JY_RET` 对应该通道。

## JY61P 运行诊断帧

固件每秒输出一次右手 JY61P 的动态状态帧：

```text
JY|ONLINE=<0/1>|ERR=<连续错误计数>|LAST=<最近读取错误位掩码>|AGE=<最近成功ACC样本年龄ms>
```

- `ONLINE` 是运行时状态，不是启动帧缓存值；连续 I2C 事务失败达到内部阈值后为 `0`，完整读取成功或恢复探测成功后回到 `1`。
- `ERR` 是连续失败事务计数；成功事务清零。
- `LAST` 位掩码：`0x01`=ACC、`0x02`=陀螺、`0x04`=角度 I2C 读取失败、`0x08`=恢复探测失败、`0x10`=姿态寄存器出现全零快照。`0` 表示最近一次事务完整成功。
- 为避免 JY61P 偶发“读取成功但六个姿态字节全零”造成 Roll/Pitch/Yaw 同时归零，三个原始角度字恰好全零的快照不作为当前有效样本；固件保留上一帧非零有效姿态。已经有过有效姿态时，`IMU` 可继续重复该最近有效值用于界面显示，但 `JY LAST=16` 明确标记它不是新姿态，客户端不得用它作报警判定。若开机后从未得到有效姿态，固件不会伪造 `IMU` 数据。单个轴为零或两个轴为零仍是正常姿态，不会被过滤。
- `AGE` 使用无符号毫秒数表示最近一次成功 ACC 样本的年龄。初始化或恢复后尚无成功 ACC 样本时固定为 `4294967295`；读失败时仍可显示此前成功样本的年龄。

该帧用于区分“设备在线但本次 ACC 无效”和“设备已动态离线”，不能把缓存的 ACC 数值当作新样本。

## 报警与 ACC 帧

JY61P ACC 单位为 `g`。读取失败或样本超过固件新鲜度窗口时，仍可输出最近值，但必须标记无效；`VALID=0` 不得作为报警输入：

```text
ACC|X=<g>|Y=<g>|Z=<g>|VALID=1
```

报警仅在 ACC 与姿态都有效且新鲜时由固件判定。确认后事件锁存，后续传感器掉线不撤销已确认事件；合法 `ALARM` 应由监护端直接按事件键处理，不应再用当前传感器健康状态拦截已锁存事件。

```text
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=1|ACTIVE=1
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=2|ACTIVE=1
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=1|ACTIVE=0
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=2|ACTIVE=0
```

`TYPE=1` 为疑似跌倒（冲击、姿态变化、静止确认），`TYPE=2` 为窗口内持续多次大变化的异常剧烈抖动。当前样机中，`TYPE=2` 需要相邻有效 ACC 三轴变化量达到 `2.0 g`，在 `1200 ms` 窗口内累计至少 10 次，且两次命中至少间隔 `50 ms`；启动、传感器恢复或一次报警解除后还须先静止 `2000 ms` 才重新武装。`TYPE=2` 只使用 JY61P ACC 判定；FLEX、MAX30102 心率/血氧和普通蓝牙命令不触发蜂鸣器报警。报警阈值是样机调参，不是医疗诊断。`BOOT+ID` 是事件键；ID 在单次启动内递增，BOOT 优先由 STM32F407 硬件 RNG 生成且不写 Flash。RNG 失败时只能使用 RAM 内回退混合值，不能宣称跨重启唯一或将其用于安全认证。

为解决 `ACTIVE=0` 消警帧在断线期间丢失后客户端保留旧报警的问题，固件还每秒发送当前报警状态快照：

```text
ALARM_STATE|BOOT=<uint32>|ACTIVE=0|ID=0|TYPE=0
ALARM_STATE|BOOT=<uint32>|ACTIVE=1|ID=<uint32>|TYPE=<1/2>
```

该帧是当前设备状态的权威快照：客户端重连后收到 `ACTIVE=0` 或新的 `BOOT` 时，应解除同一设备遗留的旧活动事件；收到 `ACTIVE=1` 时，应以其中的事件作为当前活动事件。它不替代即时 `ALARM` 事件帧，后者仍用于低延迟触发本地/监护端提示。

佩戴者当前连接的 BLE 客户端请求解除当前事件发送：

```text
ALARM_ACK:<boot>:<id>\n
```

该 ACK 由佩戴者当前 BLE 客户端发送；监护者/监护端只确认接收报警及解除回执，不向固件发送远程 ACK。ACK 必须同时匹配当前活动事件的 BOOT 和 ID；旧格式、过期 ID 和其他会话均拒绝。匹配 ACK 后固件发送对应 `ACTIVE=0`。若 ACK 或消警回执因链路丢失而重复，BLE 客户端可在有限次数内重发同一 ACK，固件对最近一次已解除的完全匹配 BOOT+ID 幂等重发同一 `ACTIVE=0`；它不会影响新的活动事件。

报警/消警优先于普通遥测。USART3 仍为 9600、8N1；`FLEX` 和已有最近有效姿态的 `IMU` 每 `250 ms` 发送，`CARE`、`PPG`、`JY`、`ACC`、`ALARM_STATE` 每 `1000 ms` 发送，`BRINGUP` 和 `UART3_RX` 每 `5000 ms` 发送。典型文本负载约 `650~700 B/s`，为 9600、8N1 理论约 `960 B/s` 留有余量；实际余量仍取决于字段长度和活动报警重发。发送 API 每次提交的完整字节单元不可被另一优先级插入；单元内部可分成多个不超过 64 字节的 HAL 传输，只有单元完成后才重新择优。当前固件的文本生产者均一次提交完整 `\r\n` 行；`BT_SendRaw` 仍支持任意原始字节，调用者若发送协议文本也应一次提交完整行。

普通蓝牙命令、灵敏度/校准按钮、FLEX 和 MAX30102 生命体征遥测不会启动蜂鸣器。只有固件判定的活动报警可以启动本地报警；ACK 仅在同时匹配当前活动事件的 `BOOT+ID` 时解除该事件并停止其蜂鸣器。小程序解析器会在遇到缺失换行而拼接的已知帧头时重新分帧，并把无法恢复的坏帧记为警告；这只保护客户端状态，不能恢复真实丢失的字节。

## USART3 RX 诊断帧

固件每 5 秒低频输出一次 USART3 接收统计，用于区分 PC 命令是否到达 MCU、HAL 接收是否因错误停止，以及完整行或 TEST ACK 是否在队列阶段丢失：

```text
UART3_RX|BYTES=<n>|LINES=<n>|LINE_DROP=<n>|RESET=<n>|LONG=<n>|ERR=<n>|ORE=<n>|FE=<n>|NE=<n>|PE=<n>|RECOVER=<n>|ARMFAIL=<n>|ACK_RETRY=<n>|ACK_DROP=<n>
```

- `BYTES`：USART3 HAL 单字节接收完成回调次数，包含随后因 UART 错误而丢弃的字节；它不是错误回调未读出字节的硬件计数。
- `LINES`：接收组帧器看到的 LF 完整行数；`LINE_DROP` 是完整行队列已满时丢弃的行数。
- `RESET`：UART 错误导致未完成行缓冲被清空的次数；已经入队的完整行不会因此删除。`LONG` 是超长输入行丢弃次数。
- `ERR` 和 `ORE/FE/NE/PE`：HAL 接收错误回调/带错误完成回调总数及错误位分类。
- `RECOVER`：发生错误或重挂接失败后成功恢复 HAL 单字节接收的次数；`ARMFAIL` 是 `HAL_UART_Receive_IT` 失败次数。
- `ACK_RETRY`：TEST ACK 因普通发送队列空间不足而重试次数；`ACK_DROP` 是 TEST ACK 暂存队列容量不足时的丢弃次数。TEST ACK 仍走普通 TX，不会进入或抢占报警优先级 TX 队列。

计数器自上电累计并按 `uint32_t` 回绕。STM32F4 HAL 对 ORE 会先结束当前 RX 并调用错误回调；回调按 HAL 提供的 `__HAL_UART_CLEAR_PEFLAG` SR/DR 序列清除硬件标志，再重新挂接单字节接收。FE/NE/PE 若仍处于 `BUSY_RX`，HAL 保留进行中的接收，固件不会重复启动或中止它。错误字节不送入文本协议组帧器；当前未完成行及其余尾部会一直丢弃到下一个 LF，再从干净行边界继续。

## 当前硬件映射

```text
USART3: PC10 TX, PC11 RX, 9600
I2C1:   PB6 SCL, PB7 SDA, JY61P 右手通道, 7-bit address 0x50
I2C2:   PB10 SCL, PB11 SDA, JY61P 左手通道, 7-bit address 0x50
I2C3:   PA8 SCL, PC9 SDA, MAX30102
ADC1:   PC3, PA1, PA2, PA3, PA4 -> L1~L5
ADC2:   PA5, PA6, PA7, PB0, PB1 -> R1~R5
Buzzer: PD14 / TIM4_CH3
```
