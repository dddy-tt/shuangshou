# 报警状态机主机测试

测试直接编译固件共用的 `Core/Src/alarm.c`，覆盖：

- 冲击、姿态变化、静止确认三步跌倒判定；
- 单次冲击不触发抖动，窗口内多次大变化才触发；
- 读取失败、重复/旧时间戳不触发；
- `uint32_t` tick 回绕计时和 Roll `±180°` 边界不误判；
- 活动事件只分配一个 ID，ACK 必须同时匹配 `BOOT` 和 `ID`；
- `ALARM` 和 `ALARM_ACK` 文本格式。

在固件目录执行：

```text
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc Core/Src/alarm.c tests/alarm_engine_test.c -lm -o tests/alarm_engine_test.exe
tests/alarm_engine_test.exe
```

该测试不连接开发板，不访问 UART、RNG、PWM 或继电器。

## Level 2 虚拟传感器输入测试

测试纯 C `test_input.c` 的命令 parser、边界检查、ENTER/APPLY/EXIT、超时恢复以及非法输入不覆盖已应用快照：

```text
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc Core/Src/test_input.c tests/test_input_test.c -lm -o tests/test_input_test.exe
tests/test_input_test.exe
```

静态接线守卫检查默认 REAL、主循环真实/虚拟分流、Keil 工程源文件和 `.ioc` 中既有 USART3 配置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/test_input_wiring_test.ps1
```

## USART3 RX recovery and end-to-end TEST command path

`usart3_rx_test.c` compiles the production `usart3_rx.c`, `bluetooth.c`, and
`test_input.c` with a small HAL mock. It exercises one-byte rearming, ORE
recovery, nonblocking FE/NE/PE handling, partial-line discard while preserving
completed lines, the full `TEST:ENTER` through `TEST:APPLY` parser path, and
rate-limited retry after a failed HAL rearm:

```text
gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc tests/usart3_rx_test.c -lm -o tests/usart3_rx_test.exe
tests/usart3_rx_test.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tests/usart3_rx_wiring_test.ps1
```

The host test does not emulate electrical line noise or replace a real STM32
HAL/USB-TTL test. The wiring guard additionally checks the bundled STM32F4 HAL
ORE handling and Keil project source registration.
