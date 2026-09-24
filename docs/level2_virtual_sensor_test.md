# Smart Glove Level 2：STM32 虚拟传感器自动测试

## 数据流

```text
PC JSON case
  -> Python Runner
  -> USB-TTL / USART3 文本 TEST 命令
  -> test_input 暂存、校验、APPLY
  -> 现有 Hand_Left/Hand_Right + JY61P_Right 运行时数据
  -> 原 Gesture / Alarm / Telemetry 业务逻辑
  -> USART3 正式 FLEX / IMU / ACC 输出
  -> Python 比对 -> PASS / FAIL
```

REAL 与 VIRTUAL 只在传感器输入边界不同。固件上电固定为 REAL；只有收到 `TEST:ENTER` 才进入 VIRTUAL，`TEST:EXIT` 或 5 秒超时恢复 REAL。

## USB-TTL 接线

测试时先断开 JDY-23，避免两个 TX 同时驱动 STM32 RX。

| STM32F407 | USB-TTL | 说明 |
|---|---|---|
| PC10 / USART3_TX | RX | STM32 输出到 PC |
| PC11 / USART3_RX | TX | PC 命令输入 STM32 |
| GND | GND | 必须共地 |

串口参数：`9600, 8N1`，3.3V TTL。不要用 USB-TTL 的 VCC 给开发板供电。

## 使用

```powershell
python -m pip install -r tools/glove_test/requirements.txt
python tools/glove_test/run_virtual_sensor_test.py --list-ports
python tools/glove_test/run_virtual_sensor_test.py --port COM5
python tools/glove_test/run_virtual_sensor_test.py --port COM5 --case tests/cases/all_bent.json
```

Runner 打开串口后默认等待 0.75 秒，再清理旧输入；`TEST:ENTER` 最多发送 3 次、每次等待 1.5 秒。只有收到 `[TEST] MODE=VIRTUAL` 才继续发送 case 命令。普通 case ACK 与 APPLY 后正式 `FLEX`、`IMU`、`ACC` 遥测仍逐项严格验证，不会把普通遥测当作 ENTER ACK。最后无论成功失败，`TEST:EXIT` 最多尝试 3 次、每次等待 1 秒；仍无 `[TEST] MODE=REAL` 时 Runner 不会报告 PASS。

可通过 `--settle-seconds`、`--enter-timeout`、`--enter-attempts`、`--exit-timeout`、`--exit-attempts` 调整等待；默认参数适用于当前 9600 baud USART3。

固件每 5 秒输出一条 `UART3_RX|...` 统计，见 [protocol.md](protocol.md#usart3-rx-诊断帧)。`BYTES=0` 表示尚无单字节接收完成回调；`BYTES` 增长但 `LINES=0` 通常指向换行未到达、接收错误或输入格式/速率问题；`ERR/RECOVER/ARMFAIL` 用于观察 HAL 错误恢复；`LINE_DROP` 和 `ACK_RETRY/ACK_DROP` 分别对应 RX 完整行和 TEST ACK 发送队列阶段。

## 软件测试

```powershell
gcc -std=c99 -Wall -Wextra -Werror -Ifirmware/stm32f407/Core/Inc firmware/stm32f407/Core/Src/test_input.c firmware/stm32f407/tests/test_input_test.c -lm -o firmware/stm32f407/tests/test_input_test.exe
firmware/stm32f407/tests/test_input_test.exe
gcc -std=c99 -Wall -Wextra -Werror -Ifirmware/stm32f407/Core/Inc firmware/stm32f407/tests/usart3_rx_test.c -lm -o firmware/stm32f407/tests/usart3_rx_test.exe
firmware/stm32f407/tests/usart3_rx_test.exe
python -m unittest tests/test_virtual_sensor_runner.py -v
powershell -NoProfile -ExecutionPolicy Bypass -File firmware/stm32f407/tests/test_input_wiring_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File firmware/stm32f407/tests/usart3_rx_wiring_test.ps1
```

## Virtual motion 整数诊断（Level 2 真机定位）

仅在 `TEST:ENTER` 后的 VIRTUAL 模式发送 `[TESTDBG]`，每 3 秒最多一组；结构布局和 `%f` 对照只在首次成功发布快照后发送一次。该诊断不参与 Runner 的 PASS 判定；PASS 仍须比对正式 `FLEX`、`IMU`、`ACC` 帧。`-2000000000` 表示浮点值无效或超出诊断转换范围。为避免 9600 波特率拥塞，正常 REAL 模式没有这些诊断帧。

使用默认 `mixed.json` 时，APPLY 后预期看到：

```text
[TESTDBG] SRC=1|APPLIED=1|SEQ=1|SR=1000|SP=-500|SY=200|SAX=0|SAY=0|SAZ=1000
[TESTDBG] PUB=<大于 0 的数字>|JR=1000|JP=-500|JY=200|JAX=0|JAY=0|JAZ=1000|ONLINE=1|EV=0|LAST=0|AV=1|ANGV=1|ASEEN=1|GSEEN=1
[TESTDBG] SZ=72|OFF_ANGLE=24|OFF_ONLINE=54|OFF_LAST=56|OFF_AV=57|OFF_ACCMS=64|FIXED=1000|FLOAT=10.00
```

`SR/SP/SY` 是 applied snapshot 姿态乘 100；`SAX/SAY/SAZ` 是加速度乘 1000。`JR/JP/JY` 与 `JAX/JAY/JAZ` 是正式 `JY61P_Right` 对应字段按同一比例转换后的值。`PUB` 只在 `TestInput_PublishMotion()` 实际写入目标结构后加 1；VIRTUAL 模式持续运行时它应增长。`APPLIED=0` 时尚未应用快照，`PUB` 不应因这次进入虚拟模式而增长。`ONLINE/EV/LAST/AV/ANGV/ASEEN/GSEEN` 分别是 online、error_streak、last_error、acc_valid、angle_valid、acc_sample_seen、angle_sample_seen。

判断顺序：

1. `APPLIED=1`，但快照整数不符合输入：检查 TEST parser / APPLY 快照。
2. 快照正确而 `PUB` 一直不增加：检查主循环 10 ms motion 调度。
3. `PUB` 增加而 `J*` 或标志位不符合快照：检查发布写入后的覆盖、结构布局和内存损坏。当前代码中真实 JY 读取/恢复只位于 REAL 分支；没有发现 I2C 回调写这个结构。
4. `J*` 正确、`FIXED=1000`，但 `FLOAT` 或正式 IMU/ACC 文本错误：优先调查当前 ARMCC5 MicroLIB 的 `snprintf("%f")` 目标机行为；不能凭主机 libc 测试断言它正常。

[ARM Compiler v5.06 MicroLIB 手册](https://documentation-service.arm.com/static/5f72dfd11b758617cd953afc?token=) 列出的不支持格式转换是 `%lc`、`%ls`、`%a`，并没有把 `%f` 列为不支持。因此这里只把浮点格式化当作需要目标机对照的假设，不能仅凭启用了 MicroLIB 就更换正式遥测格式化实现。

本机已验证可由代理自动执行 Keil CLI **Rebuild All**，使用 `E:\download\bin\STM32_Programmer_CLI.exe` 经唯一 ST-Link 正常下载 HEX、校验、复位并运行，再自行运行以下 Runner；不需要用户手动点击 Download。CubeProgrammer 仅擦除 HEX 覆盖的扇区，没有整片擦除。这是本次 Level 2 调试闭环，不是 Level 3 自动烧录框架。

```powershell
python tools/glove_test/run_virtual_sensor_test.py --port COM15
```

Runner 会直接打印 `[TESTDBG]`、正式 `IMU|...`、`ACC|...` 和最终 FAIL/PASS。若没有 `[TESTDBG]`，先核对新固件是否烧录、是否仍处于 VIRTUAL、TX 是否拥塞；不能直接推断发布函数没运行。

## 已证实的栈越界与水位

2026-09-24 真机 A/B：原 `Stack_Size=0x400` 时，快照正确且 `PUB=180`，但 JY 数据被损坏，Runner FAIL。对应 map 中栈从 `0x20003cf8` 向低地址增长，紧邻 `JY61P_Left` (`0x20003cb0`) 和 `JY61P_Right` (`0x20003c68`)。Keil 静态调用图最大深度 **1272 字节 + 不可追踪调用**，单是该深度已超过 1024 字节；`main` 帧为 840 字节。

仅将栈改为 `0x1000`、重新编译烧录后，正式 FLEX/IMU/ACC 和 Runner 均 PASS。4 KB 栈水位最初显示 `USED=1504|FREE=2592|GUARD=1`，连续三次一致。随后将仅在主循环使用的遥测文本缓冲区改为静态存储，`main` 帧降为 320 字节，静态最大深度降至 **752 字节 + 不可追踪调用**；最终真机水位 `SIZE=4096|USED=984|FREE=3112|GUARD=1`，Runner 再次 PASS。保留 4 KB 栈以覆盖中断嵌套与静态分析未知部分。

`[STACK]` 仅在 VIRTUAL 模式的低频诊断中发送；`USED` 是自启动早期填充水位后观察到的峰值，不是所有物理场景下的绝对上限。若以后启用新功能或增加中断负载，应重新测量。当前正式遥测仍由原生产路径输出，`[TESTDBG]` / `[STACK]` 不参与 Runner PASS 判定。

## Level 3 接入点

Level 3 可在 Runner 外层增加两个步骤：调用 Keil 命令行构建 `firmware/stm32f407/MDK-ARM/shuangshou.uvprojx`，再调用 ST-Link CLI 烧录生成的 HEX。Level 2 的 TEST 协议、case 文件和串口判定逻辑无需重写。
