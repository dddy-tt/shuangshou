# Smart Glove Level 3：本地硬件一键测试

Level 3 是 Level 2 Runner 外层的本地 Windows 编排器：执行软件回归、Keil Rebuild All、检查新生成的 HEX、选择唯一 ST-Link 下载并校验/复位/运行，再逐个调用现有 Level 2 Runner。它不复制 TEST 协议或遥测判断，也不修改 CubeMX、协议和小程序。

## 接线与安全

- ST-Link 保持连接 STM32，用于下载、校验和复位。
- USB-TTL 接 USART3：STM32 PC10/TX → USB-TTL RX；STM32 PC11/RX ← USB-TTL TX；GND ↔ GND。串口为 9600、8N1、3.3V TTL。
- ST-Link 与 USB-TTL 可以同时连接。测试时必须断开 JDY-23，避免 JDY-23 TX 与 USB-TTL TX 同时驱动 PC11/RX。
- USB-TTL 的 VCC 不接开发板供电；开发板使用原有稳定电源。
- `--port` 必须指定 USB-TTL 的串口号（例如 COM15）；不要把 ST-Link VCP 的 COM8 当成测试串口。

## 运行

从仓库根目录：

```powershell
python -m pip install -r tools/glove_test/requirements.txt
python tools/glove_test/test_hardware.py --port COM15
```

默认执行完整流程，并按文件名排序运行 `tests/cases/*.json`。只跑一个 case：

```powershell
python tools/glove_test/test_hardware.py --port COM15 --case tests/cases/mixed.json
```

常用可选参数：

- `--baud 9600`：USB-TTL 波特率，默认 9600。
- `--all-cases`：明确选择全部 JSON case（也是默认行为）。
- `--keil <UV4.exe>`、`--programmer <STM32_Programmer_CLI.exe>`：覆盖工具自动发现。
- `--probe-serial <ST-LINK序列号>`：多个 ST-Link 时必须指定目标序列号；单个 ST-Link 可自动选中。
- `--skip-software-tests`：仅在明确需要时跳过固件 host/Python/wiring 回归。
- `--skip-build`：只有同时 `--skip-flash` 才允许；避免把上一次遗留 HEX 当成当前固件烧录。
- `--skip-flash`：在已有固件上运行串口 cases；默认完整流程不跳过烧录。

工具可由命令行参数、环境变量 `GLOVE_KEIL` / `GLOVE_STM32_PROGRAMMER`、PATH 和常见 Windows 安装目录发现。自动发现失败时用参数或设置环境变量；不要通过修改脚本写死某台电脑路径。

## 自动流程与安全门

1. Preflight 检查 Python/pyserial、指定 COM、Level 2 Runner、case、STM32F407ZGTx Keil 工程、Keil、STM32 Programmer 和 ST-Link。
2. 执行 `alarm_engine_test`、`bluetooth_tx_test`、`jy61p_zero_filter_test`、`test_input_test`、`usart3_rx_test` 五个 firmware C host tests，`tests/test_virtual_sensor_runner.py`，以及 `alarm_wiring_test.ps1`、`test_input_wiring_test.ps1`、`usart3_rx_wiring_test.ps1`。任意失败都在 Build/Flash 前停止。
3. 对 `firmware/stm32f407/MDK-ARM/shuangshou.uvprojx` 执行 Rebuild All。要求进程成功，Keil 完整日志明确为 `0 Error(s), 0 Warning(s)`，且项目配置输出的 HEX 确实在本次 Build 后更新。旧 HEX 不会作为失败构建的回退产物。
4. Programmer CLI 按探针序列号连接。无参数时只允许自动选唯一探针；检测到多个且没有明确序列号时停止。正常编程只覆盖 HEX 涉及的区域，不做 mass erase；必须由 CLI 确认 Download/Verify 成功后才继续 Reset/Run。
5. 短暂启动稳定时间后，按顺序启动现有 `run_virtual_sensor_test.py` 测试每个 case。每个 case 仍由 Level 2 负责 ENTER retry、命令 ACK、FLEX/IMU/ACC 最新遥测收敛和 EXIT 确认；case 非零退出码使整体失败。
6. `[STACK]` 水位只作为 Level 3 健康诊断：观察到 `GUARD=0` 会使本轮失败；记录 `USED/FREE`，但不设未经验证的最低 FREE 阈值。某次没有收到低频水位帧只报告不可用，不改变 Level 2 的判定。

完整日志和汇总保存在 `artifacts/hardware_test/<时间戳>_<进程号>/`，包含 `build.log`、`flash.log`、`hardware.log`、`summary.txt` 和软件测试日志；目录已加入 `.gitignore`。成功返回 0，任一失败返回非零。失败时先查看汇总指向的阶段和对应完整日志；不要只看控制台的简短摘要。

此工具会对连接中的 F407 执行固件下载。请确认目标板和唯一 ST-Link 是本项目设备；多个探针必须显式指定序列号。它不是 GitHub Actions/远程烧录系统，也不证明传感器的物理电气行为、微信 BLE 或医疗有效性。
