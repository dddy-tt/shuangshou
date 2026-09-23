# ESP-01S MQTT 继电器最小接入说明

## 目标

让你的 ESP-01S 继电器板直接订阅我们项目的 MQTT 控制 topic：

- `shuangshou/control/light`
- `shuangshou/control/fan`

收到：

```json
{"action":"ON","source":"gesture"}
```

或：

```json
{"action":"OFF","source":"gesture"}
```

后切换继电器状态。

## 你需要准备

1. Arduino IDE 或 PlatformIO
2. ESP8266 开发板支持包
3. PubSubClient 库
4. 串口下载器
5. 3.3V 稳定供电

## 文件

示例代码：

[esp01s-mqtt-relay-example.ino](/E:/DATA/STM32/cubemx/shuangshou/docs/esp01s-mqtt-relay-example.ino)

## 先改这 4 个地方

### 1. Wi-Fi

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

### 2. MQTT topic

灯模块：

```cpp
const char* MQTT_SUB_TOPIC = "shuangshou/control/light";
const char* MQTT_STATUS_TOPIC = "shuangshou/status/light";
```

风扇模块：

```cpp
const char* MQTT_SUB_TOPIC = "shuangshou/control/fan";
const char* MQTT_STATUS_TOPIC = "shuangshou/status/fan";
```

### 3. 继电器控制脚

常见候选：

- `GPIO0`
- `GPIO2`

代码里先默认：

```cpp
const uint8_t RELAY_PIN = 0;
```

如果烧进去后 MQTT 收到了、但继电器不动作，就改成：

```cpp
const uint8_t RELAY_PIN = 2;
```

### 4. 继电器有效电平

很多 ESP-01S 继电器板是低电平吸合：

```cpp
const bool RELAY_ACTIVE_LOW = true;
```

如果表现反了：

- 发送 `ON` 却断开
- 发送 `OFF` 却吸合

就改成：

```cpp
const bool RELAY_ACTIVE_LOW = false;
```

## 烧录后串口应看到

```text
[WIFI] connected, ip=...
[MQTT] connected
[MQTT] subscribed shuangshou/control/light
```

## 先不靠手势，直接测 MQTT

先启动：

```bash
cd serial-mqtt-ai-bridge
npm install
npm start
```

然后直接发测试请求：

灯开：

```bash
curl -X POST http://localhost:8765/api/control ^
  -H "Content-Type: application/json" ^
  -d "{\"device\":\"light\",\"action\":\"ON\",\"source\":\"manual-test\"}"
```

灯关：

```bash
curl -X POST http://localhost:8765/api/control ^
  -H "Content-Type: application/json" ^
  -d "{\"device\":\"light\",\"action\":\"OFF\",\"source\":\"manual-test\"}"
```

如果继电器动作了，说明 MQTT 这条链已经通了。

## 再测手势联动

1. 在网页里保存一个 `control` 类自定义手势
2. 动作内容填：
   - `LIGHT_ON`
   - 或 `LIGHT_OFF`
3. 做出这个手势
4. 观察：
   - bridge 控制台日志
   - ESP 串口日志
   - 继电器是否吸合/释放

## 建议的最小演示顺序

1. 先只做灯
2. 跑通 `LIGHT_ON`
3. 再跑通 `LIGHT_OFF`
4. 后面再复制成风扇模块

这样最稳，不容易在比赛前把链路做复杂。
