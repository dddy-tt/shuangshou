## 本轮完成：软 I2C → 硬件 I2C 迁移（JY61P + MAX30102）

### 背景
之前三路传感器（右手 JY61P、左手 JY61P、MAX30102）全部通过 bit-bang 软 I2C 驱动，不稳定且占用 CPU 时间。本轮在不破坏蓝牙/语音/ADC/手势等既有功能的前提下，将主链切换到硬件 I2C。

### 引脚分配
| 硬件 I2C | SCL | SDA | 设备 | 改线 |
|----------|-----|-----|------|------|
| I2C1 | PB6 | PB7 | 右手 JY61P | 不动线 |
| I2C2 | PB10 | PB11 | 左手 JY61P | 原 PB8/PB9 飞过来 |
| I2C3 | PA8 | PC9 | MAX30102 | 原 PC6/PC7 飞过来 |

### 修改的文件
- **新建** `Core/Inc/i2c.h` + `Core/Src/i2c.c` — 三路 I2C 初始化（100kHz），含引脚复位+总线恢复
- **修改** `stm32f4xx_hal_msp.c` — 添加 `HAL_I2C_MspInit()`，配置 PB6/PB7/PB10/PB11/PA8/PC9 为 AF_OD (AF4)，注意 PC9 也是 AF4（不是 AF1）
- **修改** `stm32f4xx_hal_conf.h` — 取消注释 `HAL_I2C_MODULE_ENABLED`
- **修改** `Core/Src/main.c` — `SoftI2C_Init()` 改为 `MX_I2C1/2/3_Init()`
- **重写** `Core/Src/jy61p.c` — `SoftI2C_ReadBuf/ProbeReg` 替换为 `HAL_I2C_Mem_Read/IsDeviceReady`（注意：`HAL_I2C_Mem_Read` 的 DevAddress 传 8-bit 左移地址，如 0xA0，不要右移）
- **重写** `Core/Src/max30102.c` — 全部 `SoftI2C_` 调用替换为 HAL I2C 封装层
- **修改** `MDK-ARM/*.uvprojx/uvoptx` — 添加 `i2c.c` + `stm32f4xx_hal_i2c.c`
- **保留** `soft_i2c.c/.h` — 不删，留作后备

### 调试过程中踩过的坑
1. `stm32f4xx_hal_conf.h` 里 `HAL_I2C_MODULE_ENABLED` 被注释了，编译报 undefined
2. Keil 工程漏了 `stm32f4xx_hal_i2c.c` 源文件
3. `HAL_I2C_IsDeviceReady` 在 F4 上易报假 BUSY/NACK，改为直接 Mem_Read 读寄存器验证
4. JY61P 初始化时角度寄存器全零导致误判 RET=4，去掉全零检查
5. MAX30102 要加 100ms 上电延时才响应 I2C 地址
6. 地址格式：`JY61P_I2C_ADDR=0xA0` 已经是 8-bit 写地址，直接传入 HAL，不要 >> 1

### 当前状态
- ✅ 右手 JY61P — `IMU|R,P,Y` 200Hz 实时更新
- ✅ MAX30102 — `PART_ID=0x15` 初始化成功，FIFO 读取正常
- ✅ 右手 JY61P 初始化返回 `JY_R=1, RET=0`
- ✅ FLEX/蓝牙/语音/ADC 全部完好
- ❌ 左手 JY61P — 未连接（硬件 I2C2 已配好，接线后即可启用）
- 遗留：`soft_i2c.h` 仍引用 `main.h`，但主链已不再编译它
