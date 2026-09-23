# 多电器远控协议与使用说明

## 范围

远控页支持多个相互独立的 ESP-01/ESP-01S 单路继电器。每块板使用自己的唯一 ID；小程序只保存设备名称、ID、主题派生规则和本地手势绑定，不在页面保存 broker 用户名、密码或其他技术密钥。

当前实现文件：

- `miniprogram/services/device-store.js`：设备、当前选择和手势绑定的本地持久化。
- `miniprogram/services/mqtt.js`：一个 MQTT 连接，多设备主题订阅；每个设备独立维护继电器状态、pending 和可用性。
- `miniprogram/services/relay-gesture.js`：控制手势稳定判定和单目标保护。
- `miniprogram/pages/remote/`：设备添加、编辑、删除、选择、手动控制和手势绑定页面。
- `miniprogram/device-firmware/esp01-relay/`：可独立构建的 ESP-01 PlatformIO 模板。

## 设备数据和持久化

首次使用时设备库会提供一个兼容旧固件的 `light` 设备：

```text
名称：灯
唯一 ID：light
```

新设备添加时填写现场固件打印的 chipId。ID 只允许 1~64 个字母、数字、点、下划线或短横线，并且大小写不区分重复；除保留的 legacy `light` 外，内部会统一为大写 ID。设备库保存到本地 storage；当前选中的设备和手势绑定也分别持久化。删除设备会把指向它的绑定保留为禁用 tombstone，不能自动迁移到当前设备；编辑 ID 会重新派生主题并迁移已有有效绑定。

设备记录的主题由 ID 派生，不能通过页面随意输入主题：

```text
shuangshou/control/<ID>
shuangshou/status/<ID>
shuangshou/availability/<ID>
```

例如输入 `a1b2c3` 后，规范化 ID 为 `A1B2C3`，控制主题是 `shuangshou/control/A1B2C3`，状态主题是 `shuangshou/status/A1B2C3`。MQTT 主题大小写敏感；页面不会接受一套 ID 对应多套大小写主题。`light` 始终使用原有 `shuangshou/control/light` 和 `shuangshou/status/light`。

## MQTT 载荷

新模板和小程序默认使用纯文本：

```text
控制：ON 或 OFF
继电器状态：ON 或 OFF
可用性：ONLINE 或 OFFLINE
```

小程序同时兼容旧 `light` 固件：

```text
控制主题：shuangshou/control/light
状态主题：shuangshou/status/light
状态载荷：纯文本 ON/OFF，或 JSON {"state":"ON"}/{"state":"OFF"}
```

为兼容曾经的配置，MQTT 服务也能按设备设置发送 JSON `{ "action": "ON" }`，但新设备模板只需要纯文本，不应同时让同一设备接收两套控制器。

### 状态语义

- 页面初始显示“设备状态未知”，不会因为 Broker 已连接就显示设备在线。
- retained 的 `ON/OFF` 只表示 broker 中保存了历史状态；它不会清除 pending，也不会证明设备当前在线。
- retained 的 `ONLINE/OFFLINE` 也只保留为历史可用性，显示未知，不启动心跳计时，不能证明当前在线。
- 收到非 retained 的状态回报，或收到非 retained 的 `ONLINE` 心跳后，设备可用性才标记为在线；之后在心跳超时且没有新实时回报时退回未知，并清理该设备自己的计时器。
- 收到非 retained 的 `OFFLINE` 遗嘱后显示离线；MQTT 连接本身断开时，各设备回到未知。
- 指令只在对应设备收到非 retained 的相同状态回报后结束该设备自己的 pending。一个设备等待回报不会阻塞另一个设备的手动控制。

## 远控页操作

1. 打开“添加电器”，填写易识别的名称和设备端打印的 chipId。
2. 点击设备卡片选择目标；选中状态会持久化，下次进入仍恢复。
3. 先手动连接 MQTT，再点击某一设备卡片内的“闭合”或“断开”。按钮只影响该卡片对应的主题。
4. 点击手势绑定下拉框，为某个 ON/OFF control 手势选择设备；未绑定选项明确表示兼容 legacy `light`，只有用户显式选择“跟随当前选择”时才作用于当前选中的一个设备。已删除设备显示为禁用绑定，必须显式重新选择设备、跟随当前选择或清除后才改变目标。
5. 打开手势控制后，手势需保持 0.8 秒；离开页面、切换设备、状态冲突或设备待回报时不会重复发送。

手势匹配有单目标保护：同一次姿态如果命中绑定到不同设备的多个控制手势，即使动作相同也只显示冲突，不发布任何指令。一个手势记录也不会展开成多个目标。远控入口只接受客户端代理在 `app-state` 中维护的 `lastFlexAt` 作为 FLEX 新鲜度；新的 IMU/CARE 帧只更新通用帧时间，不能让旧 FLEX 姿态继续触发。

## 固件模板

模板入口：[miniprogram/device-firmware/esp01-relay](/E:/DATA/STM32/cubemx/shuangshou/miniprogram/device-firmware/esp01-relay/README.md)。它包含：

- WiFiManager 配网；
- `ESP.getChipId()` 生成唯一 ID 和独立主题；
- GPIO0 低有效继电器，上电默认 OFF；
- 纯文本 ON/OFF 控制和状态回报；
- MQTT retained 状态、遗嘱 OFFLINE、上线 ONLINE 和 15 秒心跳。

模板里的 broker 是占位符，不会自动连接公共 broker，也不会自动发出继电器指令。请只在受控环境中填入自有或获授权的 broker。公共 broker 仅限短时演示，禁止用于真实无人值守、人员安全相关或其他危险负载；正式部署至少需要 TLS、设备级账号/ACL、明确的失联策略和现场电气安全验收。

小程序的 `project.config.json` 需要在打包配置中排除 `device-firmware/`，由监护代理处理；本任务没有修改该配置文件。

## 验证边界

已提供 Node mock 测试，覆盖：设备 CRUD/选择/绑定持久化、ID 主题派生和大小写重注册、删除后的页面 tombstone、不同设备同时 pending、历史状态不确认指令、retain 可用性不冒充在线、实时心跳和超时、手势多目标冲突、`lastFlexAt` 新鲜度和 0.8 秒单次触发。

未在本环境连接真实 broker、开发板、ESP-01 或继电器，未烧录、未触发真实负载；固件模板本轮未修改，主代理已报告使用 `C:/Users/zxcvb/.platformio/penv/Scripts/platformio.exe` 完成 PlatformIO 编译。
