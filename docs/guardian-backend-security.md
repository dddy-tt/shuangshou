# 监护云函数安全与并发说明

## 范围与部署状态

本说明对应 `miniprogram/cloudfunctions/guardian/` 的云函数实现。它保持现有云函数动作名和客户端字段兼容：`createInvite`、`acceptInvite`、`getBinding`、`listEvents`、`publishEvent`、`touchStatus`、`getStatus`、`acknowledgeEvent`、`resolveEvent` 仍由同一个入口路由。

本次只改代码和部署清单，未连接微信开发者工具、未部署云函数、未修改生产数据库、未烧录固件，也没有发送真实继电器指令。`database.rules.json` 和 `database.indexes.json` 是按集合整理的人工应用清单，不会被 `wx-server-sdk` 自动上传；部署前应在目标环境逐项核对。

## 身份与命名空间

- 每次调用只使用 `cloud.getWXContext().OPENID` 作为服务端身份。请求体里的 `wearerOpenId`、`guardianOpenId`、`bindingId` 不作为授权来源。
- 新增 `guardian_accounts` 作为每个账号的唯一 claim 文档，文档 ID 为 `account_<sha256(openid)前32位>`。claim 指向唯一 `bindingId`，并保存角色、双方账号和 `wearableId` 的一致性字段。
- `guardian_bindings` 仍保存绑定关系，绑定 ID 仍由佩戴者/监护者账号确定性生成，以保持已有 API 返回值形状；事务同时建立双方账号 claim 和绑定文档。已有账号或已有绑定不能被新邀请码覆盖。
- 事件的客户端 `eventKey` 仍为 `device/boot/id`。服务端数据库文档 ID 改为 `sha256(bindingId + '/' + eventKey)` 前 32 位；事件文档另外保存并严格校验 `bindingId`、`wearerOpenId`、`wearableId`。状态文档 ID 同样包含绑定命名空间。

因此两个租户即使使用相同 `wearableId`、`boot`、`id` 和 `eventKey`，也不会共享事件/状态文档。即使异常或旧数据落入目标文档 ID，写入前仍要求现有文档的绑定、佩戴者和设备全部精确匹配。

## 绑定与邀请码并发规则

`acceptInvite` 在事务内重新读取邀请码、双方账号 claim 和确定性绑定文档，再执行检查和写入：

1. 邀请码必须匹配、未使用且 `expiresAt > now`。
2. 同一邀请码同时被多个账号接受时，只有先提交的事务能设置 `usedAt/usedBy/usedBindingId`；其他事务得到 `INVITE_INVALID`。
3. 同一账号并发接受不同邀请码时，双方事务争用同一个 `guardian_accounts` 文档；一个成功建立 claim，另一个得到 `ALREADY_BOUND`，不会产生第二条绑定。
4. 成功请求因网络重试时，原账号携带同一已使用邀请码会返回原绑定；其他邀请码不会替换已有绑定。
5. 已存在佩戴者绑定时，只能继续使用相同 `wearableId` 生成邀请；不同设备返回 `WEARABLE_MISMATCH`。已存在的佩戴者/监护者关系不能用邀请码改成另一台设备。

事件 `publishEvent` 和确认 `acknowledgeEvent` 使用事务内文档读取。事件状态只允许保持 ACTIVE 或由设备上报为 `active=false`；如果事务看到已有 `active=false`，旧的 ACTIVE 重试只返回已解除事件，不会重新激活。ACK 只写入当前绑定命名空间的事件。

`origin='device'` 只是兼容字段，服务端不把它当作设备身份凭据。事件必须先通过当前微信账号的有效绑定、佩戴者角色和服务端绑定设备匹配；服务端随后自行写入规范化的 `origin: 'device'`。当前链路仍是佩戴者小程序作为 BLE 网关上报，尚未具备硬件密钥证明，不能据此宣称能抵御被盗用的佩戴者账号。

## 事件、状态读取边界

- `listEvents` 同时按 `bindingId`、`wearerOpenId`、`wearableId` 查询，并再次过滤三项；可选的 `wearableId/deviceId` 与绑定不一致直接拒绝。
- `acknowledgeEvent` 由当前绑定命名空间计算文档 ID，并校验事件 `eventKey`、`bindingId`、佩戴者和设备；找不到时返回 `EVENT_NOT_FOUND`。
- `touchStatus` 只允许绑定佩戴者，设备必须等于绑定设备；`getStatus` 只读取当前绑定命名空间并校验归属。
- `boot`、`id` 按 uint32 校验；`alarmType` 只接受 1/2；`active` 继续接受布尔值或 0/1。客户端 eventKey 形式不变。

## 数据库规则与索引

`database.rules.json` 为五个集合提供同一默认策略：客户端 `read/create/update/delete` 全部拒绝。所有业务读写必须经过云函数；云函数使用服务端 SDK 并从 `OPENID` 派生绑定关系。规则应用后，应使用一个普通小程序账号尝试直接读写五个集合，确认均被拒绝，再通过云函数验证业务流程。

`database.indexes.json` 的必要索引为：

- `guardian_bindings.wearerOpenId`、`guardian_bindings.guardianOpenId`：旧数据兼容检查和状态回退查询。
- `guardian_invites.code`：建议唯一索引，保证邀请码代码不会对应多个文档；应用前先检查旧数据中的重复 code。
- `guardian_events(bindingId, wearerOpenId, wearableId, occurredAt desc)`：支持监护列表的归属过滤和时间倒序。
- `guardian_accounts`、`guardian_statuses` 主要通过确定性 `_id` 读取，不额外要求索引。

这些清单是控制台/CloudBase 管理操作的输入，不含环境 ID、AppSecret、MQTT 密码或用户凭证。

## 旧模板数据处理

本次不自动迁移生产数据：

- 旧 `guardian_events` 使用 `hash(eventKey)` 文档 ID，通常没有 `bindingId`；新代码不会把它们当作新命名空间事件读取，也不会原地覆盖。部署前应由管理员按账号、设备和时间人工核验后迁移或归档。
- 旧 `guardian_statuses` 使用佩戴者账号哈希，新代码从绑定命名空间读取；旧状态不会被跨绑定复用，首次新上报会建立新状态文档。
- 旧 `guardian_bindings` 没有账号 claim 时，新代码只在能唯一确定一条记录时兼容读取；发现多个记录会返回 `BINDING_STATE_CONFLICT`，不会猜测或替换设备。管理员可在停写窗口按字段核验后补建 `guardian_accounts`。
- 旧邀请码如果仍未使用，可由事务重新检查后消费；旧已使用记录不会被重置。没有 `usedBindingId` 的旧已使用记录只能在确认 `usedBy`、佩戴者、设备和绑定记录一致后做人工补字段。

## 测试与剩余验证

后端回归测试 `miniprogram/test/guardian-backend.test.js` 通过 fake `wx-server-sdk` 的真实 `index.js` 入口和内存 database 操作运行，不是把 `callFunction` 返回值 mock 成成功。覆盖：跨租户相同 eventKey、不同邀请码并发接受、解除后旧 ACTIVE、非本人 ACK、状态/设备边界、过期码、重复码、同账号重试和设备替换。

执行：

```text
node miniprogram/test/guardian-backend.test.js
node --check miniprogram/cloudfunctions/guardian/index.js
node --check miniprogram/test/guardian-backend.test.js
```

fake database 会串行化测试事务来验证唯一 claim 和条件更新逻辑，但这不等价于真实 CloudBase 多实例在目标环境的并发压测。仍未验证：微信开发者工具真实登录/openid、CloudBase 事务冲突重试行为、数据库规则/索引实际应用、线上旧数据迁移、真实 BLE/固件和任何继电器动作。
