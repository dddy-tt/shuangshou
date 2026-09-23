# Mini Program Architecture

This document describes the current product only. Previous mini program layouts and product drafts are not implementation requirements.

## Scope

The mini program lives in `miniprogram/` and must not modify `shuangshou_base_f407/`. The STM32 text protocol remains the source of truth in `docs/protocol.md` and `shuangshou_base_f407/docs/protocol.md`.

## Three Isolated Modes

The native tab bar contains exactly three modes:

1. Translation: live ten-finger bend display, JY61P pose, custom gesture library, stable 300 ms matching, and optional TTS.
2. AI rehabilitation: randomly selects from the same custom gesture library and gives per-finger correction feedback.
3. MQTT remote: publishes device commands and displays MQTT state. It never performs translation, TTS, or rehabilitation matching.

Only the safety monitor runs across mode changes. Switching modes resets business matchers, timers, and pending TTS.

## Runtime Data Flow

```text
STM32 BLE notify
  -> utils/bluetooth.js
  -> utils/protocol.js
  -> store/app-state.js
  -> current mode page only
```

`store/app-state.js` owns one BLE client and one protocol parser. Pages subscribe to the runtime and never create their own BLE connection. FLEX frames update ten values and their straight/half/full classifications. IMU frames update Roll/Pitch/Yaw and feed the cross-mode safety monitor.

## Services

- `services/gesture-matcher.js`: bend classification, gesture comparison, and the 300 ms one-shot stable matcher.
- `services/gesture-store.js`: local gesture library persistence and editing operations.
- `services/tts.js`: TTS adapter. It is unavailable until an endpoint is configured; no credentials are stored in page code.
- `services/mqtt.js`: MQTT adapter and centralized Broker/topic configuration.
- `services/safety-monitor.js`: sliding-window pose-change detection with consecutive samples and cooldown.

## Real Hardware Boundary

The current STM32 protocol provides `BOOT`, `FLEX`, `IMU`, and `BRINGUP`. It does not provide gesture IDs, TTS, MQTT, or a buzzer command parser. The mini program therefore uses real BLE telemetry for sensors, while TTS/MQTT require user configuration and the safety buzzer command is an explicit best-effort attempt that is marked unsupported until firmware implements it.

## Verification Boundary

Node tests cover protocol parsing, bend classification, gesture matching, 300 ms debouncing, and safety detection. WeChat DevTools, a real JDY-33 link, MQTT Broker, TTS endpoint, and physical buzzer remain hardware or external-service validation items.
