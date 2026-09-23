# 智能手语手套（当前跟进版本）

这是从历史工作区提取出的可复现项目树，只保留当前 STM32F407 固件、微信小程序、ESP-01S 继电器模板、监护端云函数和验证源码。

## 目录

- `firmware/stm32f407/`：STM32F407 正式 CubeMX/Keil 工程。
- `miniprogram/`：微信小程序主客户端、监护端云函数和 ESP-01S MQTT 继电器模板。
- `docs/`：协议、硬件映射、实施/测试进度及现场联调文档。

## 开发入口

- Keil 工程：`firmware/stm32f407/MDK-ARM/shuangshou.uvprojx`
- CubeMX 文件：`firmware/stm32f407/shuangshou.ioc`
- 微信开发者工具：导入 `miniprogram/`
- ESP-01S：用 PlatformIO 打开 `miniprogram/device-firmware/esp01-relay/`

## 提交边界

不提交编译输出、缓存、个人微信开发者工具配置或任何本地密钥。百度 TTS 和监护端配置均提供 example 文件；请复制为 `.local.js` 后在本机填写。

通信协议以 [docs/protocol.md](docs/protocol.md) 为准；阶段状态与真机待验证项见 [docs/progress.md](docs/progress.md)。
