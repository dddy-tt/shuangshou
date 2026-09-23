# Mini Program Progress

## 2026-09-22 - 数据流恢复、JY61P 防零与报警闭环修复（当前）

### 已完成

- 修复 JY61P 的“最近有效姿态被错误停发”链路：驱动检测到三个原始角度字全零时仍会让本次 `angle_valid=0`，确保它不进入跌倒/抽搐判定；但只要本次开机已经有过一帧有效姿态，固件会继续向 BLE 发送该最近有效 `IMU` 值，避免页面在偶发全零快照后完全失去姿态数据。
- 小程序现在把任意非零 `JY LAST` 视为当前 JY 样本异常并暂停安全判定；其中 `LAST=16` 会明确显示“姿态帧异常：页面仅展示最近一次有效姿态”。这避免界面缓存值被误当作实时运动，也不会再把 `0,0,0` 当成真实姿态展示。
- 根据真机 BLE 日志中的 `JY ONLINE 必须为 0 或 1`、`Y 无效数字` 和多类文本帧交错，定位到 USART3 9600、8N1 的文本生产负载长期接近物理线速，存在丢字节/缺失换行后帧拼接风险。
- 固件遥测已分层节流：`FLEX`/有效 `IMU` 为 250 ms，`CARE`/`PPG`/`JY`/`ACC`/`ALARM_STATE` 为 1 s，`BRINGUP` 为 5 s；报警/消警仍使用优先队列。
- 小程序协议解析器能从已知帧头恢复缺失换行导致的拼接帧，并限制破损缓冲区最大长度；无法恢复的单帧只记为警告，不再把 BLE 连接状态误标为错误。
- JY61P 三个原始姿态字同时为零时，固件不再发布 `IMU=0,0,0`，而是保留上一次非零姿态并给出 `JY LAST=16` 诊断；单轴/双轴为零仍按正常姿态处理。
- 新增每秒 `ALARM_STATE` 心跳。BLE 断开期间丢失 `ACTIVE=0` 后，重连收到设备空闲状态或新启动会话时，小程序会清理同一设备遗留的“异常抖动/跌倒”卡片和本地提示。
- 对已经由本机确认、但设备尚未连接而无法核验的旧活动报警，首页新增“隐藏本机旧提示”。它只隐藏当前手机的展示，不发送 BLE 解除命令、不改变事件的 `ACTIVE` 状态；重连后若设备仍报告活动报警会自动恢复显示，只有匹配的设备 `ACTIVE=0` 才会真正解除。
- 外部蜂鸣器现在只由 JY61P 固件确认的 `ALARM` 事件启动。移除了旧 MAX30102 HR/SpO2 直接置 SOS、绕过报警状态机而鸣叫的路径；MAX30102 数据仍保留为展示遥测。
- 对低电平触发蜂鸣器，GPIO 初始化、TIM4 切换复用、PWM 启动前三处都预置 PD14 高电平/静音比较值，缩短上电阶段误鸣窗口。

### 本轮验证

- 本次先新增“`angle_valid=0` 但已有最近有效姿态时，IMU 不得停发”和“`JY LAST=16` 必须暂停安全判定、保留既有显示姿态”回归断言；未修复代码下两项均失败，修复后通过。
- `node miniprogram/test/protocol.test.js` 通过；新增了“超长破损缓存后从最新帧头恢复”的回归用例。
- `node --test miniprogram/test/*.test.js`：26/26 通过，0 失败；`node --check` 已覆盖本次修改的状态管理和设置页脚本。
- `alarm_engine_test`、`jy61p_zero_filter_test`、`bluetooth_tx_test`、`alarm_wiring_test` 全部通过。
- `alarm-runtime.test.js` 与 `alarm-integration.test.js` 额外覆盖：已本机确认的断连旧提示可隐藏但不伪造 `ACTIVE=0`，设备重连后若仍发出同一 `ACTIVE=1` 会重新展示，而状态心跳 `ACTIVE=0` 仍能真实消警。
- 这些是 Node/GCC 主机模拟、静态接线守卫和文本协议回归；不等同于已在开发板、JDY-23 或真机微信上验证。

### 未完成与下一步真机验证

- 已在本机 Keil MDK ARMCC V5.06 update 6 对 `shuangshou/shuangshou/MDK-ARM/shuangshou.uvprojx` 执行本轮 Build：`0 Error(s), 0 Warning(s)`；日志为 `shuangshou/shuangshou/MDK-ARM/build_codex_jy_alarm_recovery_20260922.log`。这只证明目标工程可构建，尚未烧录或真机验证。
- 重新烧录后需验证：连续轻甩不会触发 TYPE=2；发生 `JY LAST=16` 时 IMU 保留最近有效值且设置页显示“姿态帧异常”；正常读取恢复后 `LAST=0`、三轴重新实时变化；断线后消警可由 `ALARM_STATE` 自动收敛；MAX30102 的随机数值不再使 PD14 鸣叫。
- 若 MCU 上电复位前蜂鸣器仍持续鸣叫，需在蜂鸣器 I/O 到其 VCC 增加约 10 kΩ 硬件上拉；这是复位高阻阶段无法由软件完全覆盖的硬件保护。

## 2026-09-18 - 二次验收进展（最终回归进行中）

> 本条记录只反映当前阶段性验收结果，不代表最终签收。工作区存在其他代理和用户改动，本轮未回滚、未提交，也未修改本文件以外的文件。

### 本轮涉及模块

- `miniprogram/`：状态帧兼容、时间戳有效性、ACK 重发；多电器删除目标不转发到其他电器；监护端后端租户隔离与事务；前台/后台轮询重试。
- 蓝牙报警发送与监护端云端链路：蓝牙优先报警队列、云端事件同步解除相关逻辑、outbox 历史事件重传、断 BLE 后的真实历史事件恢复、音频文件写入与播放时序。
- STM32 固件 `alarm.c` 状态机及其 GCC 测试。
- ESP-01 继电器 PlatformIO 模板工程。

### 当前已通过或已获得的构建/测试结果

- `miniprogram/test/*.test.js` 共 26 个测试文件全部通过。
- GCC `alarm.c` 状态机测试通过。
- 第一轮 Keil 编译结果为 `0 Error(s), 2 Warning(s)`；固件代理报告修复后为 `0 Error(s), 0 Warning(s)`，但主代理尚未复核，暂按“待复核”记录。
- 第一轮 ESP 模板 PlatformIO 编译成功。

### 仍在修复中的问题

- Kant：蓝牙优先报警队列当前可能把报警数据插入长数据帧中间；修复完成后必须验证帧边界仍完整，再确认优先级行为。
- Archimedes：监护端云端事件同步解除相关逻辑、outbox 不得伪造 `ACTIVE1`、断 BLE 后仍能重传真实历史事件，以及音频文件写入竞态仍在修复中；修复完成后需重新执行相关回归。

### 待最终回归与未验证项

- 待 Kant、Archimedes 提交修复后，重新执行全部 26 个小程序测试，并补测报警队列帧边界、监护端 outbox 重传和音频写入时序。
- 主代理需从当前工作区重新复核 Keil 编译结果，不能以固件代理自报的 `0/0` 代替复核；ESP 模板也需按最终工作树重跑 PlatformIO 编译。
- 尚未烧录 STM32 或 ESP，未部署云端，未连接真实 broker、BLE 设备、ESP-01 或继电器，未发出真实继电器指令。
- 未验证微信开发者工具/手机真机上的 BLE 建链、Notify、断线恢复、报警音频和页面前后台切换；未验证真实云端运行行为、物理报警/继电器效果或任何医疗有效性。

### 回归测试方法

1. 在仓库根目录运行全部小程序 Node 测试，例如 `node --test "miniprogram/test/*.test.js"`；确认 26 个测试文件全部通过，并重点检查状态帧/时间戳/ACK、删除路由、租户事务、前后台重试、报警队列和监护端 outbox 用例。
2. 在包含 `Core/Inc`、`Core/Src`、`tests` 的固件目录执行 `gcc -std=c99 -Wall -Wextra -Werror -ICore/Inc Core/Src/alarm.c tests/alarm_engine_test.c -lm -o tests/alarm_engine_test.exe`，再运行 `tests/alarm_engine_test.exe`，确认状态机测试通过。
3. 用 Keil MDK 对当前 STM32 工程执行一次完整 Rebuild/Build，只记录编译输出，不执行 Download/烧录；主代理复核错误和警告数量。
4. 在 `miniprogram/device-firmware/esp01-relay/` 执行 `pio run`，确认最终 ESP 模板可构建。
5. 上述代码测试和构建通过后，仍需在具备设备和部署条件时另行进行真机、云端和物理负载验收；这些不属于当前已完成的验证。

## 2026-09-16 - 控制手势触发与手势快照

- remote 页面新增手势控制开关，读取已启用 control 类 ON/OFF；保持 800ms 后发送，需松开至无匹配姿势才能再次触发。离页关闭，MQTT/BLE 未连接、数据超时或动作冲突不发送。
- 新增 services/relay-gesture.js；手势库 js/wxml 显示保存时左右十指数值，保留一位小数，缺失数据为横线。
- 静态语法与模拟保持、去重、冲突、断线测试通过；实际 BLE 手势控制继电器尚待真机验证。

## 2026-09-16 - 实机旧固件指令格式验证

- 串口确认 ESP 收到 control/light JSON 指令，但返回 unknown cmd；实际固件与本地 ESP 源码不同。
- 小程序 services/mqtt.js 默认改发纯文本 ON/OFF（待实机确认旧固件是否接受），commandFormat=json 可恢复与现有 ESP 源码兼容；状态解析兼容纯文本与 JSON。
- 未烧录 ESP，未远程触发继电器。需用户重新预览并先点断开，观察串口是否仍报 unknown cmd；回报主题尚不能从旧固件日志确定。

## 2026-09-16 - ESP-01S 远控适配

- services/mqtt.js 使用已有 MQTT.js 的 wxs 传输连接 broker.emqx.io:8084/mqtt；匹配 ESP 的 shuangshou/control/light 与 shuangshou/status/light，指令为 action ON/OFF，不保留控制消息。
- remote.js/wxml 仅展示单路继电器，收到状态才更新；保留消息标记为历史，非保留同状态回报结束等待，8 秒无回报明确提示。重复连接保护、断线清理和重新进入页面状态同步已添加。
- 语法与模拟客户端测试通过，覆盖未连接拒绝发送、主题与负载、重复连接、历史状态不确认指令、实时回报确认。未连接真实服务器发指令，未验证实际继电器。
- 用户需配置 socket 合法域名 wss://broker.emqx.io:8084；ESP 需已配网并运行已审查的程序。公共主题无身份认证，后续正式部署需专属鉴权服务器；当前固件无心跳/遗嘱，不能证明持续在线或物理触点实际状态。

## 2026-09-16 - 康复实时反馈语音

- 康复页接入百度 TTS：正确时鼓励，错误时优先播报一根需调整的手指；仅姿态不符时提示调整手腕。
- 相同提示去重、变化提示间隔至少 5 秒，新增重播按钮和语音状态。暂停、切页、重新开始取消待播提示；TTS 增加异步合成取消保护。
- 修改 rehabilitation.js/wxml、services/tts.js、新增 test/rehabilitation-speech.test.js。
- 验证：康复播报去重、延迟取消和过期回调测试，TTS 与匹配回归测试；真实手机音频仍待验证。

## 2026-09-16 - 康复与远控实时十指显示

- 新增共享 hand-monitor 组件，并接入 rehabilitation 与 remote 的 JS/WXML/JSON；左右手分区显示一位小数、进度条和弯曲状态，沿用本地校准和独立通道开关。
- 使用现有 runtime 数据订阅，隐藏页面停止提交 UI 更新；远控数据显示不依赖 MQTT 连接。
- 测试：检查组件、页面语法和配置，验证十指分组、数值边界与禁用状态。真机布局和 BLE 切页刷新待验证。
- 修改：miniprogram/components/hand-monitor/*、pages/rehabilitation/rehabilitation.{js,json,wxml}、pages/remote/remote.{js,json,wxml} 与本文档。

## 2026-07-14

### Completed

- Preserved the real STM32 text protocol parser for `BOOT`, `FLEX`, `IMU`, and `BRINGUP`.
- Added one shared runtime for BLE, protocol frames, mode state, and safety monitoring.
- Rebuilt the product around three isolated tabs: translation, AI rehabilitation, and MQTT remote control.
- Added ten-finger bend classification: `[0,30)` straight/green, `[30,70)` half/amber, `[70,100]` full/red.
- Added local custom gesture capture, edit, re-record, enable/disable, pose matching toggle, and delete.
- Added 300 ms stable gesture matching with one-shot speech trigger behavior.
- Added rehabilitation target selection from the same gesture library and per-finger correction feedback.
- Added centralized MQTT service configuration and remote device controls.
- Added cross-mode sliding-window safety detection, phone vibration, visible warning, cooldown, and best-effort buzzer command status.
- Kept `shuangshou_base_f407/` unchanged.

### Verification

- Protocol, gesture matcher, and safety monitor Node tests pass.
- All mini program JavaScript syntax checks pass.
- All mini program JSON files parse successfully.
- Real WeChat DevTools, BLE hardware, MQTT, TTS, and buzzer behavior are not verified in this environment.

## 2026-09-14 - 网站迁移与小程序主客户端（阶段 1）

### 已完成

- 完成 `web-dashboard`、`miniprogram`、协议解析、BLE 逻辑、手势、TTS、护理、康复和远控源码审计。
- 新增网站到小程序的功能迁移矩阵：`docs/web_to_miniprogram_migration.md`。
- 新增全局视觉规范：`docs/design_system.md`。
- 确认 JDY-23 正式通道为 `FFE0/FFE1`，且当前网站/内层 F407 固件的实际数据包含 FLEX、IMU、CARE、PPG。
- 确认根 `docs/protocol.md` 对基础固件仍有效，但没有覆盖内层当前固件的 CARE/PPG 扩展；本阶段未修改 STM32 或协议。

### 未完成

- 原生 BLE 的固定 FFE0/FFE1 发现、重连与日志。
- CARE/PPG 兼容解析、校准持久化、页面拆分、手势导入导出、TTS 队列与 UI 重构。

### 本阶段修改文件

- `docs/web_to_miniprogram_migration.md`
- `docs/design_system.md`
- `docs/progress.md`

### 验证与已知问题

- 本阶段为静态源码审计；未操作 STM32、网页或真机 BLE。
- 官方设计资料检索在当前工具中未返回可读内容，因此设计规则只基于已确认的通用平台原则和当前代码审计；后续以真机可读性验收为准。

## 2026-09-14 - 原生 BLE 底座（阶段 2，进行中）

### 已完成

- `utils/bluetooth.js` 已固定 JDY-23 `FFE0/FFE1`，并实现扫描、三次建链、Notify、Write、连接互斥、一次事件绑定、断线后三轮自动重连和可读错误信息。
- `utils/protocol.js` 已兼容 CARE/PPG 及诊断固件 PPG 的可选 HR/SPO2 字段。
- `store/app-state.js` 已集中保存 BLE 链路、原始帧、日志、CARE/PPG、设备和重连状态；页面不自行解析或自行建链。
- 移除未在协议中证实的自动 `BEEP:ALARM` 写入，避免伪造蜂鸣器控制能力。

### 验证

- `node --check`：protocol、bluetooth、app-state 通过。
- `protocol.test.js`、`gesture-matcher.test.js`、`safety-monitor.test.js` 通过。
- 微信开发者工具、真机 JDY-23 连接、实际 Write 和断线重连尚未验证。

## 2026-09-14 - 客户端页面与视觉结构（阶段 3/4，进行中）

### 已完成

- 新增首页 `pages/home`、手势训练 `pages/gesture-train`、手势库 `pages/gesture-library`、设备状态 `pages/settings`。
- 翻译页已重构为当前翻译结果、左右手十指弯曲度、IMU 与系统状态；手势管理已从主屏移出。
- 新增共享视觉 tokens 和 `StatusBadge`、`SectionHeader`、`FingerBar`，全局采用浅色、留白、细边线和低饱和状态色。
- 手势库支持本机搜索、启停、编辑文字、删除确认、JSON 导入和复制导出；手势训练页可以捕获当前十指和 IMU 后保存。

### 验证

- 所有 `miniprogram/**/*.js` 已通过 `node --check`；所有 JSON 可解析；协议、手势匹配和安全监控 Node 测试通过。
- 微信开发者工具对 WXML/WXSS 的编译、真机 BLE、导入文件、TTS 和高频刷新仍待验证。

### 待完成

- 将康复/远控页按新组件体系重构。
- 增加并持久化 FLEX Zero/Full、IMU Zero、启用手指和 TTS 节流。
- 在微信开发者工具和真机完成完整验收流程。

## 2026-09-14 - 校准与播报防抖（阶段 5，进行中）

### 已完成

- 新增 `services/calibration.js`，并将 FLEX Zero/Full、IMU Zero 与校准结果置于共享状态、本机存储中。
- 设置页可实际保存三类校准；翻译与训练页读取同一校准后的十指/姿态数据。
- TTS 服务增加相同文本 2200ms 去重节流；音频失败不会中断 BLE、解析或手势匹配。

### 已知问题

- 没有配置动态文本音频服务时，TTS 会明确显示未配置；这不是 BLE 失败。
- 当前康复和远控页仍是保留业务逻辑的过渡版，待按新的页面结构进一步整理。

## 2026-09-14 - 上线前审查问题修复（阶段 6）

### 已完成

- 修复手势库读取时覆盖历史 `updatedAt` 的问题；现在只在新增或编辑时更新修改时间。
- 修复同一 JSON 备份重复导入导致相同手势 ID 重复的问题；合并导入会跳过已存在的 ID。
- 首页“当前翻译结果”改为读取共享识别状态，不再固定显示“等待手势”。
- 共享状态将 BLE 原始帧、日志与解析结果合并为最多约 32ms 一次的界面通知；翻译页改为进入页面时读取一次手势库，避免每条数据帧同步读取本地存储。
- 修复 BLE 建链失败后的内部关闭连接会意外安排自动重连的问题，并新增模拟失败建链测试。
- 手势离开后会清除播报锁；再次稳定做出同一手势时可重新触发播报，实际播放仍由 TTS 的 2200ms 节流控制。

### 验证

- 全部小程序 JS 通过 `node --check`；全部 JSON 可解析；所有注册页面都具备 JS/JSON/WXML/WXSS。
- `protocol`、`gesture-matcher`、`safety-monitor`、`calibration`、`gesture-store`、`bluetooth` Node 测试通过。

### 仍待真机验证

- 微信开发者工具 WXML/WXSS 编译与手机 BLE 真机连接、断线重连、FFE1 Notify、文件导入和动态 TTS 服务。

## 2026-09-15 - 真机反馈修复：手指通道与 JY61P 诊断

### 已完成

- 设备状态页新增十根手指独立开关。关闭任一通道后，翻译、手势记录和康复匹配都会忽略该通道；界面会明确标记“已关闭”。
- 新增 JY61P 原始 R/P/Y 与 `BRINGUP` 在线/返回码诊断卡，能区分“小程序未收到数据”和“固件未读到模块”。
- 修复翻译页与手势训练页两列手指网格的最小宽度约束，避免窄屏时组件撑出卡片；手指卡文字会截断而不扩展页面宽度。

### 诊断结论

- 当前 `shuangshou_base_f407` 固件的 JY61P 驱动使用 I2C1：PB6=SCL、PB7=SDA、地址 0x50；没有通过 UART 读取 JY61P。
- 固件在 I2C 探测失败时仍会发送 `IMU|R=0.00|P=0.00|Y=0.00`，因此小程序看到连续 0 不代表 BLE/解析错误。
- 若实物 JY61P 接线是 TX/RX 串口版，必须在 STM32 工程中改为对应 UART 接收驱动后才能得到姿态数据；本次遵守小程序工作边界，未修改 STM32。

### 验证

- 手势匹配回归测试覆盖“关闭一根手指后忽略其差异”和“所有手指关闭时不允许匹配”。
- 仍需微信开发者工具与真机确认新开关、诊断卡及窄屏布局。

## 2026-09-15 - 首页快捷区真机溢出修复

### 已完成

- 根据真机截图定位首页 `quick-grid` 的两列 CSS Grid 在微信渲染端发生列宽溢出。
- 改为两列换行 Flex 布局；每张快捷卡固定占父内容区 48%，并添加最小宽度、内容裁剪与文字换行约束。
- 新增 `layout-guard.test.js`，防止快捷区回退到该 Grid 写法。

### 验证

- 静态样式守卫会验证首页快捷区使用 `flex-wrap`、48% 宽度与 `min-width: 0`。
- 仍需用户重新编译并在该手机上确认视觉结果。

## 2026-09-15 - 校准按钮小屏重叠修复

### 已完成

- 真机截图确认三个校准按钮同排显示时，单个按钮宽度不足以容纳 `FLEX Zero`、`FLEX Full`、`IMU Zero` 文本。
- 校准操作改为三个纵向全宽按钮，避免小屏重叠并提高触控面积。
- 样式守卫新增校准区必须使用纵向 Flex 与 100% 宽度按钮的检查。

## 2026-09-15 - 真机数据展示流畅度与数值格式修复

### 已完成

- 统一实时展示数值为一位小数；仅作用于页面显示，手势识别、校准和存储继续使用完整原始数值。
- 翻译页把 IMU 从整页数据刷新中拆出，以约 33ms 的轻量显示插值刷新；FLEX 未变化时不会重建左右手十张手指卡。
- BLE 原始帧仍会保存最新一帧，但调试数据日志降为每秒最多一条，减少日志数组与设置页渲染开销。
- 校准区改为强制块级纵向全宽按钮，并增加按钮间距，避免全局按钮样式或窄屏渲染导致三个按钮堆叠。

### 修改文件

- `miniprogram/utils/realtime-view.js`
- `miniprogram/store/app-state.js`
- `miniprogram/pages/translation/translation.js`
- `miniprogram/pages/gesture-train/gesture-train.js`
- `miniprogram/pages/settings/settings.js`
- `miniprogram/pages/settings/settings.wxss`
- `miniprogram/test/realtime-view.test.js`
- `miniprogram/test/translation-page.test.js`
- `miniprogram/test/layout-guard.test.js`

### 验证

- 新增实时展示测试，覆盖一位小数、空 IMU 显示为 `--`、插值起始值与过渡值。
- 新增翻译页测试，覆盖“仅姿态变化时不重新提交左右手手指列表”。
- `protocol`、`calibration`、`gesture-matcher`、`gesture-store`、`safety-monitor`、`bluetooth`、布局守卫与全部小程序 JS 语法检查通过。

### 已知限制

- 串口实测 IMU 原始帧约每 200ms 一次；小程序的 33ms 是视觉平滑，不会伪造更高的真实传感器采样率。
- 仍需在微信开发者工具重新编译并在真机确认按钮布局与 IMU 手感。

## 2026-09-16 - 翻译页 TTS 服务诊断与百度短文本合成直连

### 已完成

- 定位旧 TTS 实现的默认 endpoint 为空，因此每次播报都会稳定返回 `not-configured`。
- 验证当前小程序后台的插件市场无法检索到 `WechatSI`，因此撤回该插件声明，避免开发者工具因未授权插件阻止编译。
- 已改为百度短文本在线合成：先获取短期访问 Token，再请求音频二进制，写入小程序临时文件后通过 `InnerAudioContext` 播放。
- 增加 Token 缓存、60 字限制、重复播报节流、网络/密钥/播放失败的可读状态。
- 新增 `miniprogram/config/baidu-tts.example.js`；真实密钥放在本机 `baidu-tts.local.js`，该文件被 Git 忽略。
- 新增 TTS Node 回归测试，覆盖获取 Token、合成音频、写文件、播放与未填写密钥。

### 修改文件

- `miniprogram/services/tts.js`
- `miniprogram/pages/translation/translation.js`
- `miniprogram/test/tts.test.js`
- `miniprogram/config/baidu-tts.example.js`
- `.gitignore`

### 下一步：本机配置与真机验证

- 从示例配置复制出 `baidu-tts.local.js`，仅在本机填写百度应用的 API Key / Secret Key。
- 在小程序后台将 `https://aip.baidubce.com` 与 `https://tsn.baidu.com` 加入 request 合法域名；开发阶段可在微信开发者工具临时关闭合法域名校验。
- 重新编译后，用真机点击“播报文字”验证 Token、合成、临时文件和手机音频输出。

### 已知限制

- 当前为比赛演示的直连实现，API Key / Secret Key 会随小程序包存在泄露风险；正式发布前必须迁移到云函数或自有 HTTPS 服务。
- TTS 需要网络；没有网络或服务未配置时，BLE 数据显示和手势识别不会受影响，只会给出明确的语音失败状态。

## 2026-09-18 - 监护客户端二轮返修最终结果

- 修复监护手机无 BLE 时无法同步解除：当前绑定的认证查询返回严格 deviceConfirmed=true 的 ACTIVE=0 可同步监护端 resolved；ack-only 不解除，云确认不会发送 BLE 或解除佩戴者本机 BLE 活动状态。查询绑定上下文与事件字段分离，迟到旧绑定查询被忽略。
- outbox 首次只收到 ACTIVE=0 时直接上报 RESOLVE，不补造 ACTIVE；已有真实 ACTIVE 仍先发送。保留跨重启幂等，过滤旧 synthetic ACTIVE，拒绝已解除事件的迟到 ACTIVE。
- 实时入队继续校验在线 BLE 真实来源和绑定；持久化旧事件发送不依赖 BLE 在线。每项发送前通过服务端 getBinding 校验当前账号 bindingId/设备，覆盖同 wearableId 换账号绑定且未刷新缓存的情形；认证查询失败保留队列。
- 修复报警音慢写入期间 stop/play 竞态：共享待完成写入，成功后才缓存文件路径。
- 本代理实际执行全部 miniprogram/test/*.test.js：26 个 Node 测试文件通过，0 失败；关键回归及边界见 docs/guardian-implementation.md。这些是模拟与静态验证，不代表真机测试。
- 据主代理本轮反馈：固件已由主代理独立验证，全量 Keil Rebuild 为 0 error、0 warning，两个 C 测试通过；ESP 上轮复编成功。本代理未修改或复编固件、ESP，也未修改云函数或提交代码。
- 真机 BLE/手机音频/实际报警与云部署仍未验证；本次客户端最终补丁待主代理独立复测，不标记为主代理已复核。

## 2026-09-21 - 蜂鸣器误触发与报警按钮闭环修复

### 已完成

- 移除旧的 FLEX 弯曲变化触发 SOS/蜂鸣器路径；手指数据仍用于手势识别，不再参与跌倒或异常抖动报警。
- JY61P 异常抖动检测曾改为上电或通信恢复后先连续静稳 1500 ms 才启用，并把变化阈值调整为 1.2 g、1 秒内 6 次；该历史样机值已在本文件顶部的 2026-09-22 当前补丁中进一步提高到 2.0 g、1200 ms 内 10 次且命中最小间隔 50 ms。
- 固件新增动态 `JY|ONLINE|ERR|LAST|AGE` 状态帧，小程序优先采用该帧显示 JY61P 在线、连续错误、错误位和样本年龄；旧 BRINGUP 帧不会覆盖动态状态。
- “确认并停止本地提示”只确认事件并停止当前手机的报警音，不发送 BLE，也不解除固件活动事件。
- “请求设备解除”仅限佩戴者当前已连接设备，发送精确的 `ALARM_ACK:<boot>:<id>\n`；同一事件只允许一个进行中的请求，失败后可重试。
- 小程序只有收到同一设备、同一 BOOT/ID/TYPE 的 `ACTIVE=0` 才显示已解除；旧会话、其他设备或普通云端确认不能解除固件报警。
- 两个按钮增加发送中、等待设备回执、失败可重试、已解除等明确状态；监护者端没有设备解除权限。

### 验证

- 固件报警状态机测试通过：`alarm_engine_test: all checks passed`。
- 固件蓝牙发送队列测试通过：`bluetooth_tx_test: all checks passed (16 HAL chunks)`。
- 固件接线静态守卫通过：`alarm_wiring_test: all checks passed`。
- Keil 完整编译通过：`0 Error(s), 0 Warning(s)`。
- 主代理复跑小程序全部 26 个 Node 测试文件：26/26 通过，0 失败。
- 主代理检查小程序 64 个 JavaScript 文件：语法全部通过。

### 已知限制与真机待测

- 上述结果属于编译、静态和主机模拟验证；必须重新烧录 STM32 固件并重新编译小程序后，才能验证真实蜂鸣器、JDY-23 写入和 `ACTIVE=0` 回执。
- 当前报警阈值是样机演示初值，不是医学阈值；真机测试应先记录普通动作和目标异常动作的 ACC 数据，再基于数据调整。

## 2026-09-22 - JY61P 三轴偶发同时归零修复

### 根因与修复

- 复现确认 JY61P 驱动过去只检查 `HAL_I2C_Mem_Read()` 是否返回成功；一次成功但六个姿态寄存器字节全零的快照会被立即发布，导致 Roll/Pitch/Yaw 同时跳到 0，下一帧再跳回正常值。
- 新增姿态全零去毛刺：三个原始姿态字同时为零时标记 `LAST=0x10` 并保留上一帧；该快照不会作为真实零姿态发布。
- 非零姿态仍立即发布；单轴或双轴为零是合法姿态；I2C 真失败继续使用原有 `0x04` 错误位。

### 验证

- 新增生产驱动级主机测试 `jy61p_zero_filter_test.c`，修复前稳定复现“正常→全零→正常”，修复后验证重复全零快照也不会覆盖最后有效姿态。
- `alarm_engine_test`、`bluetooth_tx_test`、`alarm_wiring_test` 与新增 JY61P 测试全部通过。
- 本轮目标工程仍需在本机 Keil 中重新 Build；历史编译结果不能替代本次固件改动验证。

### 真机待测

- 必须重新烧录本次固件；真机串口应在偶发毛刺时看到 `JY ... LAST=16`，但 `IMU` 三轴继续保持上一帧，不再跳 0。
