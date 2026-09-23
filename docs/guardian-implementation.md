# 监护与真实报警客户端实现说明

> 本文记录本轮客户端实现和与固件代理协调的接口。`docs/protocol.md` 仍是仓库协议唯一事实来源，待主代理与固件代理冻结后同步；本轮没有修改固件工程或云函数目录。

## 接口兼容约定

客户端消费以下报警链路：

```text
ACC|X=<g>|Y=<g>|Z=<g>|VALID=0/1\r\n
ALARM|BOOT=<uint32>|ID=<uint32>|TYPE=1/2|ACTIVE=0/1\r\n
ALARM_ACK:<boot>:<id>\r\n
```

`TYPE=1` 是疑似跌倒，`TYPE=2` 是异常抖动；`ID` 在本次 `BOOT` 会话内递增，事件唯一键是 `deviceId/BOOT/ID`。`ACTIVE=0` 必须携带与活动事件相同的 `BOOT/ID/TYPE`。客户端发送 ACK 时由 BLE 写服务补齐 `\r\n`，重试不会改变 `BOOT/ID`。

客户端同时兼容两种 `BRINGUP`：

```text
BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1
BRINGUP: JY_R=1,JY_L=1,JY_R_RET=0,JY_L_RET=0,MAX=1,MAX_RET=0,MAX_PART=0x57,MAX_HAL_ERR=0,ADC1=1,ADC2=1,DEG=0
```

旧格式保留 `jy/jyRet/adc1/adc2/beep`，当前固件双 JY 格式额外保留左右 JY、MAX 和 `DEG` 诊断字段，并提供聚合 `jy/jyRet` 别名。`jyValid` 只在 `BRINGUP` 更新，实际 `ALARM` 已是固件锁存并确认的真实事件，不受当前 JY 诊断状态门控。`ACC VALID` 只更新 `acc` 和 `accHealth` 诊断；`VALID=0` 不产生报警。`IMU` 姿态角不再用于客户端抽搐判定，`CARE` 仍是演示遥测，不进入真实报警或监护上报。

## 本地报警链路

`utils/protocol.js` 逐行解析原始 BLE 文本；`store/app-state.js` 只在真实 `ALARM` 帧进入 `services/safety-monitor.js` 和 `services/alarm-runtime.js`。运行时按 `deviceId/BOOT/ID` 去重，统一合并 BLE 与监护副本：

- 活动事件触发一次本地提示；重复 ACTIVE 不重复播放。
- “确认”只记录确认人并停止本地报警音，不改变事件的 `active` 状态。
- 监护者可以确认，但不能调用远程解除；`ack` 或普通云端回执不能改变活动状态。监护端只有在当前账号的已认证绑定设备匹配、且 `listEvents` 返回的服务端 `deviceConfirmed === true` 时，才把佩戴者已经上报的 `ACTIVE=0` 同步为监护端本地 `resolved`。
- runtime 将 `cloudConfirmed` 与 `deviceConfirmed` 分开：前者只影响监护端本地展示和声音，后者仍只表示当前佩戴者 BLE 收到的固件回帧；云确认不会发送 BLE，也不会解除佩戴者手机本地状态。`eventPublic` 不含 `bindingId`，设备匹配依赖本次认证查询对应的当前 binding 上下文，不能信任任意 `source/origin` 字段。
- 佩戴者本机 BLE 活动事件只有当前连接设备的匹配 `ACTIVE=0` 才能结束；历史事件、旧设备事件不能借新连接发送 ACK。
- `services/alarm-ack.js` 对同一事件 single-flight，写入失败或等待超时只做有限次数重试，始终发送同一个 `ALARM_ACK:<boot>:<id>`，并在收到当前设备 `ACTIVE=0` 前不清理运行时活动状态。

`services/alarm-audio.js` 在本机生成短 PCM WAV，不依赖 TTS 或网络，使用系统音量和静音策略。它用 generation 令牌取消 stop 之前未完成的文件准备，用 single-flight 避免并发创建多个 context；文件写失败会清除失败缓存，播放错误会停止、销毁 context，并通过结果和错误回调反馈。

## 监护 outbox 与生命周期

`services/guardian-outbox.js` 使用微信本地存储保存有界队列，事件保留设备、bindingId、BOOT、ID、类型、活动状态、时间和来源。已有真实 ACTIVE 时，同一事件按 `ACTIVE → RESOLVE` 发送：

- 本地网络失败不删除队列项；重试次数和下一次退避时间持久化。
- 若已有真实 `ACTIVE` 队列项，仍先发送 `ACTIVE` 再发送 `RESOLVE`；若首次恢复时只有 `ACTIVE=0`，直接发送 `RESOLVE`，不伪造 `ACTIVE=1`。两种路径都支持成功后的本地幂等去重。
- 达到单次前台 flush 的工作上限后保留剩余项；队列满时返回 `outbox-full`，不静默丢掉安全事件。
- 没有前台保证：应用 `onShow` 刷新角色/绑定、启动监护者轮询并触发 outbox flush；`onHide` 停止轮询。恢复后再次回到前台或手动刷新才继续发送。
- 新鲜 BLE 事件入队前校验真实 BLE 来源、当前在线设备和当前 binding；发送持久化历史项时按服务端刷新确认的当前账号 bindingId 与 wearable/device 校验，不依赖 BLE 仍在线。换绑或设备不匹配只跳过并保留，不向错误账号/设备发报文。
- 已建立绑定但缺少服务端返回的 `bindingId` 时也视为不可发送；持久化恢复会丢弃 `eventKey` 与设备/BOOT/ID 不一致的损坏条目。

每条 outbox 实际发送前调用认证 `getBinding`，复核服务端角色、bindingId 和设备；查询失败保留队列，不回退到缓存。相同 wearableId 换到另一 bindingId 也禁止发送旧事件。监护轮询在查询前取得认证 binding 上下文，返回时复核本地 binding 未变化；旧绑定查询迟到会被忽略。runtime 的云确认权限由 app-state 私有上下文与授权回调提供，事件输入自行声明 `source`、`deviceConfirmed`、`cloudConfirmed` 不获得消警权限。

音频准备优先复用进行中的 writePromise，文件访问/写入成功后才缓存路径；慢写期间 stop 后再次 play 仍等待同一写入。旧版持久化队列中的 synthetic ACTIVE 恢复时过滤，真实 RESOLVE 保留发送。

全局 runtime 还提供 `lastFlexAt`，且只有收到 FLEX 帧才更新；ACC、IMU、ALARM、BRINGUP 和 CARE 不会冒充手部新鲜度。远控页面可据此判断手势数据是否过期。

## 认证后端边界

`miniprogram/cloudfunctions/guardian/` 是另派代理维护的微信云开发模板，不是已部署服务，本轮保持未编辑。客户端只读取 `config/guardian.example.js` 和未提交的 `config/guardian.local.js`；环境 ID 是配置标识，不是密钥。没有环境 ID、绑定或当前设备匹配时，页面明确显示未配置/未绑定，不连接公共 MQTT，也不把 mock 数据当真实监护事件。

模板设计目标是使用微信登录身份、一次性限时邀请码、角色校验、绑定归属校验、最小事件数据和服务端幂等写入；它不能替代硬件身份认证，也没有在本轮部署。实际云开发开通、集合索引、云函数上传和两个账号绑定必须由用户在开发者工具/控制台完成。

## 用户开通步骤（本轮未执行）

1. 创建微信云开发环境，把环境 ID 写入本地未提交的 `config/guardian.local.js`。
2. 在微信开发者工具中把 `miniprogram/cloudfunctions` 配置为云函数根目录，为 `guardian` 安装依赖并部署。
3. 创建模板要求的绑定、邀请码、事件和在线状态集合及查询索引。
4. 用两个已登录账号验证邀请码一次性使用、角色权限、设备归属、重复 ACTIVE/RESOLVE 和离线恢复。

`project.config.json` 将 `cloudfunctions/` 和 `device-firmware/` 排除在小程序客户端包之外。本轮没有部署、烧录、连接真机、实际触发继电器、读取或输出任何密钥。

## 测试边界

`miniprogram/test/` 的 protocol、safety、alarm、guardian 测试覆盖：旧/新 BRINGUP 解析、ACC 诊断、JY 无效仍接收 ALARM、事件去重、ack-only 不消警和 cloud-confirmed resolve、本地 WAV 取消/单 flight/慢写入 stop/play/失败重试、ACK 超时重试和旧设备隔离、有界 outbox 的直接 RESOLVE 幂等与真实 ACTIVE→RESOLVE 恢复、监护轮询 stop-start、BLE 断开后的合法历史补发和账号换绑隔离，以及从原始 ACC/ALARM 到 app-state、声音、ACK 和 outbox 的集成注入链路。

这些证据均为 Node 模拟测试，不等价于微信开发者工具、云函数线上运行、手机系统音量、BLE 真机往返或 STM32 实机报警验证。

2026-09-18 二轮返修最终回归：逐一执行 `miniprogram/test/*.test.js`，26 个测试文件全部通过。关键新增断言覆盖监护 active→cloud-confirmed resolve、ack-only/未确认快照不解除、错误设备与迟到绑定查询隔离、佩戴者 BLE 事件不被云端解除、首次直接 RESOLVE/跨重启幂等/旧 synthetic 清理、慢写 stop/play、断 BLE 重启后历史补发、相同设备换 bindingId 未刷新仍禁止发送、认证查询失败保留队列。最终补丁尚待主代理独立复测。
