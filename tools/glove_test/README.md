# STM32 Virtual Sensor Test Runner

安装串口依赖：

```powershell
python -m pip install -r tools/glove_test/requirements.txt
```

列出串口：

```powershell
python tools/glove_test/run_virtual_sensor_test.py --list-ports
```

运行默认 mixed 案例：

```powershell
python tools/glove_test/run_virtual_sensor_test.py --port COM5
```

指定案例：

```powershell
python tools/glove_test/run_virtual_sensor_test.py --port COM5 --case tests/cases/all_bent.json
```

测试前必须断开 JDY-23，让 USB-TTL 独占 USART3。接线：STM32 PC10 TX → USB-TTL RX，STM32 PC11 RX ← USB-TTL TX，GND ↔ GND；9600、8N1、3.3V TTL。不要连接 USB-TTL 的 VCC 给开发板供电。
