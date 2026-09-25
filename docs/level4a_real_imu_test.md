# Level 4A：真实 JY61P 自动健康采集

## 范围与安全

本工具验证右手真实 JY61P 的 `I2C1 → jy61p.c → JY61P_Right → IMU/ACC` 路径。默认运行会复用 Level 3 的 Keil、唯一 ST-Link 探测、Flash/Verify/Reset/Run 工具，然后通过 USB-TTL（默认 COM15）静止采集 15 秒。

- 允许发送且只发送一次 `TEST:EXIT`，必须收到 `[TEST] MODE=REAL` 才采集。
- 禁止 `TEST:ENTER/FLEX/IMU/ACC/APPLY`。程序不会发送 BLE、MQTT 或继电器命令。
- 测试期间 JDY-23、ESP-01S 应断开，继电器负载保持断电；不运行报警动作测试。
- 默认不要求或触发任何手部/板卡运动。`--motion-check` 才会在人按回车后提示缓慢倾斜，不做跌落或剧烈摇晃。
- 左手 JY、协议字段、传感器量程及报警阈值均不在本测试范围内。

## 运行

默认自动构建、唯一 ST-Link 烧录校验复位、REAL 模式确认及 15 秒采集：

```powershell
python tools/glove_test/test_real_imu.py --port COM15
```

采集时间可设为 10–20 秒。`--skip-build --skip-flash` 可在明确需要复用板上既有固件时只采集；跳过 Build 时必须同时跳过 Flash，避免烧录旧 HEX。可传 `--keil`、`--programmer`、`--probe-serial` 指定工具/目标探针。

可选、需要真人配合的缓慢姿态变化检查：

```powershell
python tools/glove_test/test_real_imu.py --port COM15 --motion-check
```

## 自动判定

脚本完整保留原始串口日志，再逐窗口统计，而不是按一帧通过。默认静态验收检查：

- 至少收到 `BRINGUP JY=1,JY_RET=0`，并在全窗口收集足够的 JY/JYDBG/IMU/ACC 样本；ONLINE 比率至少 90%。
- ACC 有效率至少 90%，ANGLE 有效率至少 80%；三种样本都必须出现。单个 `ANGLE_ZERO` 被单独计数，不当作 I2C 错误或立即失败。
- `ERR` 不超过驱动的连续离线阈值 3；`LAST` 只能使用 `0x1f` 内定义位。ACC/GYRO/ANGLE 累计 I2C 错误不超过 3，恢复失败必须为 0，恢复尝试最多 1 次。
- 已见样本的年龄不得是未初始化哨兵，最大 AGE 不超过固件 120 ms 新鲜度窗口。
- IMU/ACC 正式遥测为有限值且处于驱动合理量程；用 `ACC_RAW × 16/32768` 与相邻正式 ACC 对照，误差不超过 0.002 g。静止有效 ACC 的中位模长需在 0.5–1.5 g，允许桌面倾斜。
- `READS` 推导的读取速率至少 50 Hz；JY/JYDBG/ACC 最大内部遥测间隙不超过 2.5 s、IMU 不超过 1.5 s。
- 状态/valid 标志取值合法；栈至少观测两次，所有 `GUARD=1`、`FREE>0`；出现活动报警时立即停止并且不自动确认。

阈值用于当前样机的静止健康测试，不是医疗安全认证。若判定失败，先用日志区分启动探测、I2C 各寄存器读取、样本陈旧、遥测调度和原始/换算路径；没有原始数据证据时不改 scale。

## 本轮真机结果（2026-09-25）

- 对比 I2C1 100 kHz 与 50 kHz。100 kHz 期间曾观察到在线率 87.5%、ACC/GYRO/ANGLE I2C 错误累计增加及恢复探测；另一个 15 秒窗口在线/样本有效率为 100%，但 GYRO 错误增加 23 次。因此单个短窗口不能代表稳定。
- 将右手 JY61P 所在的 I2C1 降为 50 kHz 后，保持 PB6/PB7、I2C 传感器地址、读取周期、数据比例和正式遥测格式不变；I2C2/I2C3 仍为 100 kHz。完整项目回归及随后独立 REAL 采集均通过。
- 15 秒 REAL 采集中确认 `BRINGUP JY=1,JY_RET=0`；JYDBG/IMU/ACC/STACK 分别收到 16/61/16/5 帧。ONLINE、ACC 有效率、ANGLE 有效率均为 100%，最大样本 AGE 为 2 ms，读取速率约 99.6 Hz；窗口内 ACC/GYRO/ANGLE I2C 错误、HAL 错误及恢复计数增量均为 0。
- 原始 ACC 与正式 ACC 不一致数为 0；静止重力模长中位数 0.9956 g。正式 IMU 有限且在量程内（该次静止读数 Roll=11.76、Pitch=10.66、Yaw=125.03）；最小栈剩余 3312 bytes，所有守卫值为 1。无格式错误、活动报警或虚拟模式。
- 结论限于本次硬件、接线和采集窗口：50 kHz A/B 结果消除了本次窗口内的错误，支持 I2C 通信裕量不足的假设，但不能单凭软件采集证明唯一物理根因。
- Level 4A 新增的 13 项 Python 单测通过；随后 `python tools/test_all.py --port COM15` 完整回归中 Python unittest 共 47/47，通过并得到 `PROJECT REGRESSION PASS`。详细原始结果保存在 `artifacts/real_imu/20260925_234914_648665/` 和 `artifacts/hardware_test/20260925_234816_417494_32504/`。
- `--motion-check` 未运行：静态健康验收不要求移动设备；如果要验证轴对慢速倾斜的响应，需另由人手动执行可选动作测试。

## 产物

每次结果写入 `artifacts/real_imu/<时间戳>/`：

- `serial.log`：命令方向和所有采集到的原始串口行。
- `build.log`、`flash.log`、`stlink_enumeration.txt`：实际工具调用证据（对应步骤执行时创建）。
- `summary.json`、`summary.txt`：样本统计、各项 PASS/FAIL、构建/烧录信息。

静态健康通过后，如用户需要确认角度轴对真实轻微转动有响应，再单独运行 `--motion-check`。全项目标准回归仍由 `python tools/test_all.py --port COM15` 执行；真实 JY 测试不被强制并入它。
