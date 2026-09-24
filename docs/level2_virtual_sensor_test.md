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

Runner 逐条等待 STM32 ACK，APPLY 后必须同时看到且比对通过正式 `FLEX`、`IMU`、`ACC` 三种遥测，最后无论成功失败都会尝试发送 `TEST:EXIT`。

## 软件测试

```powershell
gcc -std=c99 -Wall -Wextra -Werror -Ifirmware/stm32f407/Core/Inc firmware/stm32f407/Core/Src/test_input.c firmware/stm32f407/tests/test_input_test.c -lm -o firmware/stm32f407/tests/test_input_test.exe
firmware/stm32f407/tests/test_input_test.exe
python -m unittest tests/test_virtual_sensor_runner.py -v
powershell -NoProfile -ExecutionPolicy Bypass -File firmware/stm32f407/tests/test_input_wiring_test.ps1
```

## Level 3 接入点

Level 3 可在 Runner 外层增加两个步骤：调用 Keil 命令行构建 `firmware/stm32f407/MDK-ARM/shuangshou.uvprojx`，再调用 ST-Link CLI 烧录生成的 HEX。Level 2 的 TEST 协议、case 文件和串口判定逻辑无需重写。
