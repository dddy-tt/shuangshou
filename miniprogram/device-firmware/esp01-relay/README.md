# ESP-01 继电器设备模板

这是小程序多电器功能对应的单路 ESP-01/ESP-01S 模板。每块板使用自己的 `ESP.getChipId()` 作为设备唯一 ID，因而不会和另一块板共用控制或状态主题。

## 主题和载荷

设备启动后会把芯片 ID 转成大写十六进制，例如 `A1B2C3`。小程序中添加设备时，唯一 ID 填这个值。模板自动生成：

```text
shuangshou/control/A1B2C3       # 订阅控制
shuangshou/status/A1B2C3        # 回报 ON/OFF，retained
shuangshou/availability/A1B2C3  # ONLINE/OFFLINE，retained
```

控制载荷只接受纯文本 `ON` 和 `OFF`。状态回报也只发送纯文本 `ON` 或 `OFF`；上线/遗嘱/心跳使用 `ONLINE` 和 `OFFLINE`。小程序会把 retained 的继电器状态标为历史状态，不把它误报成当前在线。

## 安全默认值

模板中的 `MQTT_HOST` 是占位符 `YOUR_MQTT_BROKER_HOST`，因此不会自动连接公共 broker，也不会替你发出任何继电器指令。只有在你明确填写自有或获授权的 broker 后，设备才会尝试连接。

公共 broker 仅限短时演示，且没有项目级身份隔离；禁止把它用于真实无人值守、人员安全相关或其他危险负载。正式使用应采用自有 broker、TLS、设备级账号/ACL，并在通电前确认继电器默认断开。

## Wi-Fi 配网

模板使用 WiFiManager：

1. 第一次上电时，连接名称类似 `Shuangshou-A1B2C3` 的配置热点。
2. 在配置页填写现场 Wi-Fi。
3. 代码中的 Wi-Fi 密码不会写入小程序，也不要把真实密码提交到仓库。

## 需要配置的代码项

在 `src/main.cpp` 中由现场负责人填写：

```cpp
static const char* MQTT_HOST = "YOUR_MQTT_BROKER_HOST";
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_USERNAME = "";
static const char* MQTT_PASSWORD = "";
```

不要把 broker 密码、Wi-Fi 密码或其他密钥填入小程序页面。若 broker 使用 TLS，需要根据现场证书和 PubSubClient 配置另行加固，本模板默认只适合受控演示网络。

## 继电器 GPIO

默认配置：

```cpp
static const uint8_t RELAY_PIN = 0;       // GPIO0
static const bool RELAY_ACTIVE_LOW = true;
```

多数 ESP-01S 继电器板是低有效，`ON` 时 GPIO0 输出低电平。GPIO0 同时是启动绑带脚，必须确认继电器板的上拉、供电和下载接线；如果板型不是 GPIO0 控制，需由硬件人员改为正确 GPIO 并重新检查上电默认断开。模板上电先写入非激活电平并执行 `OFF`。

## 遗嘱、状态和心跳

MQTT 连接使用遗嘱：异常掉线时 broker 将向 availability 主题保留发布 `OFFLINE`。连接成功发布 `ONLINE` 和当前继电器状态；在线期间每 15 秒发布一次 `ONLINE` 心跳。小程序只在收到实时状态/心跳时显示设备在线，Broker 在线本身不代表继电器设备在线。

## 构建边界

本目录可以作为独立 PlatformIO 工程打开：

```text
pio run
```

本次任务只提供模板和静态代码，未连接开发板、未烧录、未触发继电器，也未连接 broker。父级小程序的 `project.config.json` 需要由监护代理在 `packOptions.ignore` 中排除 `device-firmware/`，避免把固件工程作为小程序资源打包；本任务不修改该配置文件。
