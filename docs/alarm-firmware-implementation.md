# 报警固件实现说明

本文档描述 `shuangshou/shuangshou` 中的报警固件实现。阈值是样机演示调参值，不代表医疗验证或临床性能。

## 协议

原有 `BOOT`、`FLEX`、`IMU`、`CARE`、`PPG` 等文本帧保留。每帧仍使用 USART3 的 `\r\n` 结尾；报警相关新增帧如下。MAX30102 的 HR/SpO2 仅用于展示，不会直接触发外部蜂鸣器或活动报警。

JY61P 加速度已经按 `g` 输出，新增遥测帧为：

```text
ACC|X=<g>|Y=<g>|Z=<g>|VALID=1\r\n
```

当最近一次加速度读取失败或样本超过新鲜度窗口时，仍可输出最近值，但 `VALID=0`；该样本不会进入报警状态机。

运行中还会输出右手 JY61P 的紧凑诊断帧：

```text
JY|ONLINE=<0/1>|ERR=<连续错误计数>|LAST=<最近读取错误位掩码>|AGE=<最近成功ACC样本年龄ms>\r\n
```

`ONLINE` 来自运行时读写/恢复状态，不使用启动诊断缓存；`LAST` 位 `0x01/0x02/0x04/0x08/0x10` 分别表示 ACC、陀螺、角度 I2C 读取、恢复探测、被丢弃的全零姿态快照；没有成功 ACC 样本时 `AGE=4294967295`。恢复只重建 HAL I2C 控制器，不发送未经手册确认的传感器复位命令。三个原始姿态字同时为零的快照始终不发布，保留上一帧非零姿态；单轴或双轴为零不受影响。

启动诊断通过与遥测相同的 USART3 普通队列发送，兼容监护代理所需的字段：

```text
BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1,MAX=0,MAX_RET=1,DEG=1
```

其中 `JY/JY_RET` 表示当前报警输入右手 JY61P，逗号后的详细字段可被忽略。该帧在
初始化后发送一次，并由主循环周期重发；客户端不能依赖一次启动帧保存传感器状态。

活动报警帧为：

```text
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=1|ACTIVE=1\r\n
```

其中 `TYPE=1` 是疑似跌倒，`TYPE=2` 是异常剧烈抖动；`ACTIVE=0` 使用相同的 `BOOT`、`ID`、`TYPE` 表示解除。

固件还会每秒发送当前活动状态，用于重连后的状态收敛：

```text
ALARM_STATE|BOOT=<uint32>|ACTIVE=0|ID=0|TYPE=0\r\n
ALARM_STATE|BOOT=<uint32>|ACTIVE=1|ID=<uint32>|TYPE=<1/2>\r\n
```

客户端收到同一设备的 `ACTIVE=0` 或新 `BOOT` 后，必须清理遗留的旧活动报警；即时 `ALARM` 事件帧仍负责低延迟提示。

佩戴者当前连接的 BLE 客户端请求解除当前活动事件时发送：

```text
ALARM_ACK:<boot>:<id>\n
```

该 ACK 由佩戴者当前 BLE 客户端发送；监护者/监护代理只确认接收报警及解除回执，不向固件发送远程 ACK。固件只接受同时匹配当前启动会话号和当前活动事件号的 ACK。旧格式 `ALARM_ACK:<id>`、过期事件号、其他启动会话号、溢出或带额外字段的输入均拒绝；接收 `\r\n` 也兼容。活动事件每 1 秒重发一次，直到匹配 ACK 或固件复位。

匹配 ACK 后，固件保留最近一次已解除的 `BOOT+ID`。若该事件的相同 ACK 因链路丢帧而再次到达，固件幂等地再次发送同一条 `ACTIVE=0`；若消警帧入队失败，则保持待发送状态并在后续 100 ms 任务重试。旧事件 ACK 不会停止或修改之后的新活动事件。

### 会话号和事件号

- STM32F407 启动后使用片上硬件 RNG 有界读取一个随机值，再与芯片 UID、启动时钟扰动等混合，强制避开 0。
- RNG 读取最多轮询 256 次，不会因 RNG 故障卡死；不可用时使用 UID/时钟/栈地址的 RAM 内回退混合值。回退路径不是密码学随机数，不能保证跨复位唯一；监护代理不得把 `BOOT` 当作密钥或安全认证凭据。
- 会话号和事件号均只存在 RAM，不使用 Flash 计数器，因此不会产生 Flash 磨损。
- 每个会话内事件 `ID` 从 1 递增；`BOOT` 与 `ID` 的组合才是监护代理的事件键，可避免复位后再次从 1 分配造成旧 ACK 碰撞。
- 为兼容现有监护代理对 `BOOT:` 旧启动帧的逐行解析，当前不额外发送未冻结的 `BOOT|SESSION` 行；会话号在第一条 `ALARM` 帧的 `BOOT` 字段中报告。

## 判定逻辑

报警状态机在 `Core/Src/alarm.c` 中实现，不访问 HAL、不延时、不轮询外设。主循环以 10 ms 任务调用它；JY61P 读取也在独立的 10 ms 任务执行。三组 I2C 向量读取完成后只取一次 `sys_tick_ms`，因此一份 ACC/姿态快照使用一致的完成时间；tick 和状态机的耗时计算均采用无符号差，支持 `uint32_t` 回绕。

### 数据有效性

报警同时要求 ACC 和姿态读取成功，且各自时间戳距当前调度 tick 不超过 120 ms。读取失败、无效标志、NaN/无穷大、重复时间戳、倒退时间戳和过期数据都会被丢弃；无效期间会清除瞬态候选，不会用旧数据触发报警。

### 疑似跌倒（TYPE=1）

必须依次满足以下条件：

1. 加速度模长达到 `2.5 g` 冲击阈值；姿态基准取冲击前一帧。
2. 冲击后 `1500 ms` 内 Roll/Pitch 合成变化达到 `45°`。
3. 姿态变化后 ACC 模长保持 `0.75 g`～`1.25 g`，相邻 ACC 变化不超过 `0.20 g`，相邻 Roll/Pitch 变化不超过 `8°`，持续 `1000 ms` 才确认。

确认后只产生一个活动事件。解除前不会因后续样本分配新的事件号；解除后仍需满足 `1500 ms` 安静时间才允许同一检测器重新武装。

### 异常剧烈抖动（TYPE=2）

启动、ACC 掉线或一次事件解除后，检测器必须先获得 `2000 ms` 的静止样本（模长约 `0.75 g`～`1.25 g`，相邻变化不超过 `0.20 g`）才重新武装。相邻有效 ACC 的三轴差值达到 `2.0 g` 才记一次大变化；两次命中至少间隔 `50 ms`，必须在 `1200 ms` 窗口内累计至少 10 次才触发。因此启动跳变、普通甩手、缓慢运动和单次冲击不会直接触发，持续快速反复的大幅变化仍会触发。该阈值是样机调参，不是医疗诊断。

`CARE` 中的演示心率、血氧、FALL、SOS 字段只用于原有演示遥测，不作为上述两类报警的输入。FLEX 数据只用于手指数据和手势，不再进入蜂鸣器报警路径；普通蓝牙命令和按钮也不会启动蜂鸣器，ACK 只解除匹配的活动事件。

## 输出与安全策略

- PD14 使用现有 TIM4_CH3，计数频率为 1 kHz，报警时比较值 500（约 50% PWM）；静音时比较值 1000（ARR=999，对外保持高电平）。TIM4_CH1/CH2 仍只供原有双手振动，不改其配置和比较值。
- GPIO 初始化阶段、TIM4 切入复用输出后、PWM 使能前都会预置 PD14 高电平；报警状态机在初始化完成前不运行输出。若模块在 MCU 复位的高阻窗口仍持续鸣叫，需要在模块 I/O 加硬件上拉（例如 10 kΩ 到其逻辑电源），软件不能保证复位前的引脚电平。
- 本地蜂鸣器最多鸣叫 30 秒；超时只关闭本地声音，活动报警仍按协议周期重发，直到佩戴者当前 BLE 客户端发送匹配 ACK。
- USART3 保持 9600、8N1 兼容。普通遥测使用 1024 字节中断队列，报警/消警使用独立 256 字节高优先级队列；每次 HAL 发送最多 64 字节，但每次 `BT_SendRaw`/`BT_SendString`/报警发送 API 提交的完整 TX 单元不可被另一优先级插入，只有该单元全部发送后才重新择优。物理 ring 尾部只造成同一单元的下一段传输，不改变优先级边界。`FLEX` 和有效 `IMU` 每 250 ms 发送，`CARE`、`PPG`、`JY`、`ACC`、`ALARM_STATE` 每秒发送，`BRINGUP` 每 5 秒发送；典型文本负载约 600~650 B/s，低于 9600、8N1 约 960 B/s 的理论线速。队列不足时整帧/整单元拒绝并返回状态，遥测在下一周期重试，活动报警和消警分别保留/重试。
- TX 入队在关中断区内完成整单元复制后一次性发布 `head`；`busy`、队列来源和 `inflight` 长度在调用 `HAL_UART_Transmit_IT` 前已发布，TX 完成 ISR 只推进对应队列。RX 使用 4 行完整文本队列，主循环取行时与 RX ISR 互斥，避免多个 ACK 或文本命令互相覆盖。

## 主机测试

在实际固件目录 `shuangshou/shuangshou` 执行：

```text
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc Core/Src/alarm.c tests/alarm_engine_test.c -lm -o tests/alarm_engine_test.exe
tests/alarm_engine_test.exe
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc tests/bluetooth_tx_test.c -o tests/bluetooth_tx_test.exe
tests/bluetooth_tx_test.exe
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc tests/jy61p_zero_filter_test.c -lm -o tests/jy61p_zero_filter_test.exe
tests/jy61p_zero_filter_test.exe
PowerShell -ExecutionPolicy Bypass -File .\tests\alarm_wiring_test.ps1
```

当前证据：`alarm_engine_test: all checks passed`；`bluetooth_tx_test: all checks passed`；`jy61p_zero_filter_test: all checks passed`；`alarm_wiring_test: all checks passed`。后者直接编译真实 `Core/Src/bluetooth.c`，以 mock HAL 完成回调覆盖长于 64 字节的 FLEX 行、ring wrap 中途报警、报警等待当前完整 TX 单元，以及队列空间不足时整单元拒绝。

每次变更 `main.c`、`gpio.c` 或 `tim.c` 后，必须在本机 Keil MDK 中 Build 实际工程 `shuangshou/shuangshou/MDK-ARM/shuangshou.uvprojx`；历史 `.axf/.hex` 或旧日志不能作为本次改动的构建证据。本轮已使用 ARMCC V5.06 update 6 重新 Build，结果为 `0 Error(s), 0 Warning(s)`，日志为 `MDK-ARM/build_codex_jy_alarm_recovery_20260922.log`；尚未执行 Download/烧录。

## 尚未验证

尚未连接开发板，也未烧录、实际触发继电器或读取硬件密钥。因此以下内容仍需硬件/监护代理联调：JY61P 真实 ACC 与姿态读取及断线恢复、片上 RNG 多次复位会话号差异、USART3/蓝牙收发、ACK 往返、PD14 低电平触发无源模块的实际极性和声音、30 秒本地静音以及 TIM4 其他通道不受影响。
