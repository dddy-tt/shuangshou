# 智能手套网站迁移至微信小程序

> 审计日期：2026-09-14。本文以当前 `shuangshou/web-dashboard/src/`、`miniprogram/` 与两套现存 F407 固件源码为依据；不以旧说明文档猜测功能。

## 结论与边界

- 后续正式客户端为微信小程序，实时数据只走微信原生 BLE，不依赖网页 Bridge、WebSocket 或 Windows Web Bluetooth。
- JDY-23 已被手机/平板实测为 BLE 外设，UART 服务为 `FFE0`、特征值为 `FFE1`；`FFE1` 同时具备 Notify 和 Write。
- 本迁移不改 STM32 帧格式。小程序必须容忍 BLE 分包、合包及不同固件版本的可选帧。
- 护理区保留为**演示数据/状态提示**，不表示医疗测量或诊断。
- 微信基础库没有通用的离线动态文本 TTS。现有小程序的 TTS 服务需要合法 HTTPS 音频服务配置后才可播放；未配置时必须明确提示且不影响 BLE、识别和页面刷新。

## 网站已确认功能清单

| 网站功能 | 已确认源码位置 | 迁移策略 |
| --- | --- | --- |
| Web Bluetooth 连接、断开、FFE0/FFE1 Notify/Write | `hooks/useBleGlove.ts` | 改为原生 `wx.*Bluetooth*` API；固定优先 FFE0/FFE1 |
| FLEX、IMU、CARE、PPG 解析及 BLE 分包缓冲 | `hooks/useBleGlove.ts` | 迁入共享 `utils/protocol.js`，一次解析、多页订阅 |
| 10 指显示、启用手指、FLEX Zero/Full、IMU Zero | `components/SensorMonitorView.tsx`、`App.tsx` | 翻译页显示，设置页维护本地校准/启用配置 |
| 手语翻译、当前结果、置信度、历史、浏览器播报 | `components/SignTranslationPanel.tsx`、`services/browserSpeech.ts` | 翻译页 + 本地手势匹配；TTS 改为小程序音频服务 |
| 自定义手势保存、编辑、删除、静态/动态快照、测试播报 | `components/CustomGestureManager.tsx`、`App.tsx` | 训练页采集、手势库页管理、本地存储与导入导出 |
| AI 康复目标、倒计时、差异反馈、统计 | `components/TrainingTaskCard.tsx`、`ResultCard.tsx`、`StatsCard.tsx` | 保留为康复页面；本地静态匹配可用，Bridge/AI 反馈为可选 |
| 护理 HR/SpO2/跌倒/SOS 演示与提醒 | `components/CareMonitoringPanel.tsx` | 设置/调试中的演示护理卡；明确非医疗用途 |
| MQTT 家电控制、控制手势 | `components/IotCard.tsx`、`App.tsx` | 保留远控页；未配置 MQTT 时只显示演示状态，不能声称真机已控 |
| Bridge/WebSocket 与 HTTP 自定义手势接口 | `hooks/useWebSocket.ts`、`App.tsx` | 不作为主实时通路；小程序先本地运行，日后可单独接后端 |

## 当前小程序已有能力

| 能力 | 现有位置 | 状态 |
| --- | --- | --- |
| 全局共享状态 | `store/app-state.js` | 已有，需扩展 |
| 蓝牙扫描/连接/Notify/Write | `utils/bluetooth.js` | 有基础，尚未固定 FFE0/FFE1、无断线重连 |
| BOOT/FLEX/IMU/BRINGUP 解析 | `utils/protocol.js` | 有基础，缺 CARE/PPG 与可选字段兼容 |
| 手势三档状态与 300ms 稳定匹配 | `services/gesture-matcher.js` | 可复用 |
| 本地手势 CRUD | `services/gesture-store.js` | 有基础，缺完整字段、导入导出与页面分层 |
| 翻译/康复/远控三页 | `pages/translation`、`rehabilitation`、`remote` | 逻辑雏形存在，视觉和信息架构需重构 |
| HTTP 音频 TTS 封装 | `services/tts.js` | 默认未配置，不能宣称可播报 |

## 尚未迁移或需重构的功能

1. 主控首页、设备连接页、设置/调试页、手势训练页、手势库页；现有翻译页职责过载。
2. 原生 BLE 的 JDY-23 精确发现、连接互斥、重复注册防护、`FFE0/FFE1` 校验、断线检测、自动重连、可读错误日志。
3. `CARE`、`PPG` 及 PPG 携带 `HR/SPO2` 的兼容解析。
4. 客户端 FLEX Zero/Full、IMU Zero、启用手指的持久化。
5. 自定义手势名称/分类/姿态容差/启用状态/导入/导出；静态手势本地识别与播报去重。
6. 动态手势可保存和展示，但网页的动态匹配由 Bridge 完成；小程序不应在未实现前假称支持实时动态识别。
7. TTS 的实际音频服务配置、播放节流和失败反馈。
8. 统一设计 tokens、组件、不同屏幕安全区适配和比赛演示级页面层级。

## 实际 BLE 数据协议

所有文本帧以 `\r\n` 结束。小程序按文本流累计，不能假定一次 Notify 恰好是一帧。

| 帧 | 格式 | 使用 |
| --- | --- | --- |
| FLEX | `FLEX|L1=0|...|L5=0|R1=0|...|R5=0` | 十路弯曲度，范围 0–100 |
| IMU | `IMU|R=0.00|P=0.00|Y=0.00` | Roll/Pitch/Yaw |
| CARE | `CARE|HR=0|SPO2=0|FALL=0|SOS=0` | 护理演示状态 |
| PPG | `PPG|IR=0|RED=0|VALID=0` | PPG 原始观测；诊断固件可能追加 `HR`、`SPO2` |
| BOOT | `BOOT:...` | 固件标识 |
| BRINGUP | `BRINGUP: ...` | 旧基础固件诊断帧 |

现存 `shuangshou_base_f407` 只明确发 BOOT/FLEX/IMU/BRINGUP；当前网站和内层 `shuangshou/shuangshou/Core/Src/main.c` 还发 CARE/PPG。由于现场使用的模块已由手机收到了 CARE/PPG，客户端按后者优先并兼容前者。

## 小程序实现映射

| 目标能力 | 小程序实现位置 |
| --- | --- |
| 全局连接、数据流和 UI 状态 | `store/app-state.js` |
| 原生 BLE 生命周期 | `utils/bluetooth.js` |
| 字节流与协议帧解析 | `utils/protocol.js` |
| 校准、手势匹配、存储、TTS | `services/calibration.js`、`services/gesture-*.js`、`services/tts.js` |
| 连接控制中心 | `pages/home/` |
| 比赛主展示 | `pages/translation/` |
| 手势采集流程 | `pages/gesture-train/` |
| 手势库/导入导出 | `pages/gesture-library/` |
| 康复、远控、护理演示、调试 | `pages/rehabilitation/`、`pages/remote/`、`pages/settings/` |
| 通用视觉组件 | `components/status-badge/`、`device-card/`、`section-header/`、`finger-bar/` 等 |
| 设计 tokens | `styles/tokens.wxss` |

## 实施顺序

1. 完成审计、迁移矩阵与设计系统。  
2. 重构共享协议、BLE 服务、状态管理；在设置页能看到原始帧和链路日志。  
3. 建立首页和翻译主页面，显示十指、IMU、BLE、识别和校准值。  
4. 完成手势采集、库管理、静态匹配、导入导出和去重。  
5. 接入/配置 TTS，保留康复、远控和护理演示的真实能力边界。  
6. 做 JS/JSON/路径静态检查；用微信开发者工具和真机完成 BLE、TTS、手势现场验收。
