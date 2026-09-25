#!/usr/bin/env python3
"""Collect and assess real right-hand JY61P telemetry over USB-TTL COM15.

The only command sent to the STM32 is one TEST:EXIT to establish REAL mode.
No virtual sensor commands, BLE, MQTT, or relay commands are used.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from statistics import median
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware" / "stm32f407"
PROJECT_FILE = FIRMWARE / "MDK-ARM" / "shuangshou.uvprojx"
ARTIFACT_ROOT = ROOT / "artifacts" / "real_imu"
REAL_ACK = "[TEST] MODE=REAL"
EXIT_ACK_TIMEOUT_S = 2.0
ANGLE_ZERO_BIT = 0x10
KNOWN_ERROR_MASK = 0x1F
HAL_I2C_ERROR_BITS = {
    0x01: "BERR", 0x02: "ARLO", 0x04: "AF", 0x08: "OVR",
    0x10: "DMA", 0x20: "TIMEOUT", 0x40: "SIZE",
    0x80: "DMA_PARAM", 0x100: "INVALID_CALLBACK",
}
NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"


class RealImuError(RuntimeError):
    """A setup, collection, or health-check failure."""


def _number(value: str) -> float:
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("non-finite numeric field")
    return result


def _pipe_fields(line: str, prefix: str) -> dict[str, str] | None:
    if not line.startswith(prefix):
        return None
    payload = line[len(prefix):]
    fields: dict[str, str] = {}
    for item in payload.split("|"):
        if not item or "=" not in item:
            raise ValueError(f"malformed {prefix} field: {item!r}")
        key, value = item.split("=", 1)
        if key in fields:
            raise ValueError(f"duplicate {prefix} field: {key}")
        fields[key] = value
    return fields


def _required(fields: dict[str, str], *keys: str) -> None:
    missing = [key for key in keys if key not in fields]
    if missing:
        raise ValueError("missing fields: " + ", ".join(missing))


def _int_field(fields: dict[str, str], key: str, *, low: int = 0,
               high: int = 0xFFFFFFFF) -> int:
    value = int(fields[key], 10)
    if not low <= value <= high:
        raise ValueError(f"{key} out of range: {value}")
    return value


def _int_vector(fields: dict[str, str], key: str, count: int = 3) -> tuple[int, ...]:
    parts = fields[key].split(",")
    if len(parts) != count:
        raise ValueError(f"{key} must contain {count} integers")
    values = tuple(int(part, 10) for part in parts)
    if any(value < -32768 or value > 32767 for value in values):
        raise ValueError(f"{key} has a value outside signed int16")
    return values


def _counter_vector(fields: dict[str, str], key: str, count: int = 3) -> tuple[int, ...]:
    parts = fields[key].split(",")
    if len(parts) != count:
        raise ValueError(f"{key} must contain {count} counters")
    values = tuple(int(part, 10) for part in parts)
    if any(value < 0 or value > 0xFFFFFFFF for value in values):
        raise ValueError(f"{key} has a value outside uint32")
    return values


def _float_vector(values: tuple[int, ...], scale: float) -> tuple[float, ...]:
    return tuple(value * scale for value in values)


@dataclass
class Capture:
    """Parsed records with monotonic timestamps; raw lines are logged separately."""

    bringup: list[tuple[float, dict[str, int]]] = field(default_factory=list)
    jy: list[tuple[float, dict[str, int]]] = field(default_factory=list)
    debug: list[tuple[float, dict[str, Any]]] = field(default_factory=list)
    imu: list[tuple[float, tuple[float, float, float]]] = field(default_factory=list)
    acc: list[tuple[float, tuple[float, float, float, int]]] = field(default_factory=list)
    stack: list[tuple[float, dict[str, int]]] = field(default_factory=list)
    malformed: list[str] = field(default_factory=list)
    active_alarm: str | None = None
    virtual_mode_seen: bool = False

    def add_line(self, line: str, at: float) -> str | None:
        """Parse one STM32 line; return the matched frame type, if any."""
        text = line.strip()
        if not text:
            return None
        if text.startswith("[TEST] MODE=VIRTUAL"):
            self.virtual_mode_seen = True
            return "TEST"
        if text.startswith("ALARM|") or text.startswith("ALARM_STATE|"):
            if re.search(r"(?:^|\|)ACTIVE=1(?:\||$)", text):
                self.active_alarm = text
            return "ALARM"

        try:
            if text.startswith("BRINGUP:"):
                fields: dict[str, str] = {}
                for item in text.partition(":")[2].split(","):
                    key, sep, value = item.strip().partition("=")
                    if sep:
                        fields[key.strip()] = value.strip()
                _required(fields, "JY", "JY_RET")
                self.bringup.append((at, {
                    "JY": _int_field(fields, "JY", high=1),
                    "JY_RET": _int_field(fields, "JY_RET", high=2),
                }))
                return "BRINGUP"

            fields = _pipe_fields(text, "[JYDBG] ")
            if fields is not None:
                _required(fields, "ONLINE", "ERR", "LAST", "ACC_RAW", "GYRO_RAW",
                          "ANGLE_RAW", "AV", "ANGV", "ASEEN", "GSEEN", "TSEEN",
                          "AAGE", "GAGE", "TAGE", "READS", "I2CERR", "HALERR",
                          "ZERO", "REC")
                errors = _counter_vector(fields, "I2CERR")
                hal_errors = _counter_vector(fields, "HALERR", count=4)
                recovery = _counter_vector(fields, "REC")
                record = {
                    "ONLINE": _int_field(fields, "ONLINE", high=1),
                    "ERR": _int_field(fields, "ERR", high=255),
                    "LAST": _int_field(fields, "LAST", high=255),
                    "ACC_RAW": _int_vector(fields, "ACC_RAW"),
                    "GYRO_RAW": _int_vector(fields, "GYRO_RAW"),
                    "ANGLE_RAW": _int_vector(fields, "ANGLE_RAW"),
                    "AV": _int_field(fields, "AV", high=1),
                    "ANGV": _int_field(fields, "ANGV", high=1),
                    "ASEEN": _int_field(fields, "ASEEN", high=1),
                    "GSEEN": _int_field(fields, "GSEEN", high=1),
                    "TSEEN": _int_field(fields, "TSEEN", high=1),
                    "AAGE": _int_field(fields, "AAGE"),
                    "GAGE": _int_field(fields, "GAGE"),
                    "TAGE": _int_field(fields, "TAGE"),
                    "READS": _int_field(fields, "READS"),
                    "I2CERR": errors,
                    "HALERR": hal_errors,
                    "ZERO": _int_field(fields, "ZERO"),
                    "REC": recovery,
                }
                self.debug.append((at, record))
                return "JYDBG"

            fields = _pipe_fields(text, "JY|")
            if fields is not None:
                _required(fields, "ONLINE", "ERR", "LAST", "AGE")
                self.jy.append((at, {
                    "ONLINE": _int_field(fields, "ONLINE", high=1),
                    "ERR": _int_field(fields, "ERR", high=255),
                    "LAST": _int_field(fields, "LAST", high=255),
                    "AGE": _int_field(fields, "AGE"),
                }))
                return "JY"

            if text.startswith("IMU|"):
                match = re.fullmatch(
                    rf"IMU\|R=({NUMBER})\|P=({NUMBER})\|Y=({NUMBER})", text
                )
                if match is None:
                    raise ValueError("malformed IMU frame")
                self.imu.append((at, tuple(_number(value) for value in match.groups())))
                return "IMU"

            if text.startswith("ACC|"):
                match = re.fullmatch(
                    rf"ACC\|X=({NUMBER})\|Y=({NUMBER})\|Z=({NUMBER})\|VALID=([01])", text
                )
                if match is None:
                    raise ValueError("malformed ACC frame")
                values = tuple(_number(value) for value in match.groups()[:3])
                self.acc.append((at, (*values, int(match.group(4)))))
                return "ACC"

            fields = _pipe_fields(text, "[STACK]|")
            if fields is not None:
                _required(fields, "SIZE", "USED", "FREE", "GUARD")
                self.stack.append((at, {
                    key: _int_field(fields, key, high=0xFFFFFFFF)
                    for key in ("SIZE", "USED", "FREE", "GUARD")
                }))
                return "STACK"
        except (ValueError, OverflowError) as error:
            if text.startswith(("BRINGUP:", "[JYDBG]", "JY|", "IMU|", "ACC|", "[STACK]|")):
                self.malformed.append(f"{text}: {error}")
                return "MALFORMED"
            raise
        return None


def _max_gap(samples: list[tuple[float, Any]]) -> float | None:
    if len(samples) < 2:
        return None
    times = [sample[0] for sample in samples]
    return max(right - left for left, right in zip(times, times[1:]))


def _ratio(numerator: int, denominator: int) -> float:
    return numerator / denominator if denominator else 0.0


def assess(capture: Capture, duration: float, real_acknowledged: bool) -> dict[str, Any]:
    """Return measured values and independent acceptance checks."""
    debug = [record for _, record in capture.debug]
    # JY and JYDBG are emitted in the same diagnostic cycle. Prefer the
    # detailed status stream so duplicate frames do not overweight one cycle.
    online_samples = capture.debug if capture.debug else capture.jy
    jy_online_values = [record["ONLINE"] for _, record in online_samples]
    online_ratio = _ratio(sum(jy_online_values), len(jy_online_values))
    acc_ratio = _ratio(sum(record["AV"] for record in debug), len(debug))
    angle_ratio = _ratio(sum(record["ANGV"] for record in debug), len(debug))
    bringup_ok = any(row["JY"] == 1 and row["JY_RET"] == 0 for _, row in capture.bringup)

    known_values = capture.jy + capture.debug
    # The driver marks ONLINE=0 on the third consecutive read error, then
    # recovery-probe failures can continue saturating the uint8 streak. A
    # streak >3 while offline is diagnostic failure evidence, not RAM corruption.
    illegal_state = any(
        (row["ONLINE"] == 1 and row["ERR"] > 3)
        or (row["LAST"] & ~KNOWN_ERROR_MASK)
        for _, row in known_values
    )
    validity_keys = ("AV", "ANGV", "ASEEN", "GSEEN", "TSEEN")
    invalid_flags = any(
        row[key] not in (0, 1)
        for _, row in capture.debug for key in validity_keys
    )
    sample_seen = {
        "ACC": any(row["ASEEN"] == 1 for row in debug),
        "GYRO": any(row["GSEEN"] == 1 for row in debug),
        "ANGLE": any(row["TSEEN"] == 1 for row in debug),
    }

    finite_imu = bool(capture.imu) and all(
        all(math.isfinite(value) and -180.0 <= value <= 180.0 for value in values)
        for _, values in capture.imu
    )
    finite_acc = bool(capture.acc) and all(
        all(math.isfinite(value) and -16.0 <= value <= 16.0 for value in values[:3])
        for _, values in capture.acc
    )

    age_values: list[int] = []
    stale_after_seen = False
    for row in debug:
        for seen_key, age_key in (("ASEEN", "AAGE"), ("GSEEN", "GAGE"), ("TSEEN", "TAGE")):
            age = row[age_key]
            if row[seen_key]:
                if age == 0xFFFFFFFF:
                    stale_after_seen = True
                else:
                    age_values.append(age)
    for _, row in capture.jy:
        if row["AGE"] != 0xFFFFFFFF:
            age_values.append(row["AGE"])
    max_age = max(age_values) if age_values else None

    read_rate: float | None = None
    if len(capture.debug) >= 2:
        first_at, first = capture.debug[0]
        last_at, last = capture.debug[-1]
        span = last_at - first_at
        if span > 0 and last["READS"] >= first["READS"]:
            read_rate = (last["READS"] - first["READS"]) / span

    latest_debug = debug[-1] if debug else None
    first_debug = debug[0] if debug else None

    def counter_delta(key: str, count: int) -> tuple[int, ...] | None:
        if first_debug is None or latest_debug is None:
            return None
        first_values = first_debug[key]
        last_values = latest_debug[key]
        if len(first_values) != count or len(last_values) != count:
            return None
        if any(last < first for first, last in zip(first_values, last_values)):
            return None
        return tuple(last - first for first, last in zip(first_values, last_values))

    error_counts = latest_debug["I2CERR"] if latest_debug else (0, 0, 0)
    first_error_counts = first_debug["I2CERR"] if first_debug else None
    error_delta = counter_delta("I2CERR", 3)
    latest_hal_errors = latest_debug["HALERR"] if latest_debug else (0, 0, 0, 0)
    recovery_counts = latest_debug["REC"] if latest_debug else (0, 0, 0)
    first_recovery_counts = first_debug["REC"] if first_debug else None
    recovery_delta = counter_delta("REC", 3)
    bus_error_total = sum(error_delta) if error_delta is not None else None
    zero_count = latest_debug["ZERO"] if latest_debug else 0
    hal_error_counts = {name: 0 for name in HAL_I2C_ERROR_BITS.values()}
    for _, row in capture.debug:
        for code in row["HALERR"]:
            for bit, name in HAL_I2C_ERROR_BITS.items():
                if code & bit:
                    hal_error_counts[name] += 1

    raw_vectors_ok = bool(debug) and all(
        all(-32768 <= value <= 32767 for key in ("ACC_RAW", "GYRO_RAW", "ANGLE_RAW")
            for value in row[key])
        for row in debug
    )
    # Formal ACC and raw ACC are emitted in the same 1 Hz diagnostic block.
    acc_consistent_samples = 0
    acc_mismatch_samples = 0
    for debug_at, row in capture.debug:
        previous_acc = next(
            ((at, values) for at, values in reversed(capture.acc) if at <= debug_at), None
        )
        if previous_acc is None or debug_at - previous_acc[0] > 1.5:
            continue
        expected = _float_vector(row["ACC_RAW"], 16.0 / 32768.0)
        actual = previous_acc[1][:3]
        acc_consistent_samples += 1
        if any(abs(a - e) > 0.002 for a, e in zip(actual, expected)):
            acc_mismatch_samples += 1

    acc_magnitudes = [math.sqrt(sum(value * value for value in values[:3]))
                      for _, values in capture.acc if values[3] == 1]
    gravity_median = median(acc_magnitudes) if acc_magnitudes else None

    gaps = {
        "JY": _max_gap(capture.jy),
        "JYDBG": _max_gap(capture.debug),
        "IMU": _max_gap(capture.imu),
        "ACC": _max_gap(capture.acc),
        "STACK": _max_gap(capture.stack),
    }
    stack_ok = len(capture.stack) >= 2 and all(
        row["GUARD"] == 1 and row["SIZE"] > row["USED"]
        and row["FREE"] == row["SIZE"] - row["USED"]
        for _, row in capture.stack
    )
    stack_free_min = min((row["FREE"] for _, row in capture.stack), default=None)
    error_streak_max = max((row["ERR"] for _, row in known_values), default=None)
    max_jy_gap_ok = gaps["JY"] is not None and gaps["JY"] <= 2.5
    max_imu_gap_ok = gaps["IMU"] is not None and gaps["IMU"] <= 1.5
    max_acc_gap_ok = gaps["ACC"] is not None and gaps["ACC"] <= 2.5
    max_dbg_gap_ok = gaps["JYDBG"] is not None and gaps["JYDBG"] <= 2.5

    checks = {
        "Mode REAL confirmed": real_acknowledged and not capture.virtual_mode_seen,
        "Bringup JY=1/JY_RET=0": bringup_ok,
        "JY status sampling": len(capture.debug) >= max(5, int(duration * 0.5)),
        "JY online ratio >= 90%": online_ratio >= 0.90,
        "ACC valid ratio >= 90%": acc_ratio >= 0.90,
        "Angle valid ratio >= 80%": angle_ratio >= 0.80,
        "ACC/GYRO/ANGLE samples seen": all(sample_seen.values()),
        "Finite/ranged IMU": finite_imu and len(capture.imu) >= 10,
        "Finite/ranged ACC": finite_acc and len(capture.acc) >= 5,
        "Freshness <= 120 ms": max_age is not None and max_age <= 120 and not stale_after_seen,
        "I2C errors/recovery stable": (
            error_delta is not None and bus_error_total is not None
            and bus_error_total <= 3
            and recovery_delta is not None
            and recovery_delta[0] <= 1 and recovery_delta[2] == 0
        ),
        "Legal status/mask values": not illegal_state and not invalid_flags,
        "Raw vectors present": raw_vectors_ok,
        "ACC raw/formal scale agrees": (
            acc_consistent_samples >= 3 and acc_mismatch_samples == 0
        ),
        "ACC gravity magnitude sanity": (
            gravity_median is not None and 0.5 <= gravity_median <= 1.5
        ),
        "100 Hz poll progress": read_rate is not None and read_rate >= 50.0,
        "Telemetry cadence": max_jy_gap_ok and max_dbg_gap_ok and max_imu_gap_ok and max_acc_gap_ok,
        "Stack guard/watermark": stack_ok,
        "No malformed sensor frames": not capture.malformed,
        "No active alarm": capture.active_alarm is None,
    }
    return {
        "checks": checks,
        "metrics": {
            "duration_s": duration,
            "bringup_samples": len(capture.bringup),
            "jy_samples": len(capture.jy),
            "debug_samples": len(capture.debug),
            "imu_samples": len(capture.imu),
            "acc_samples": len(capture.acc),
            "stack_samples": len(capture.stack),
            "online_ratio": online_ratio,
            "acc_valid_ratio": acc_ratio,
            "angle_valid_ratio": angle_ratio,
            "samples_seen": sample_seen,
            "max_age_ms": max_age,
            "stale_after_seen": stale_after_seen,
            "i2c_error_counts_at_window_start": first_error_counts,
            "i2c_error_counts_at_window_end": error_counts if latest_debug else None,
            "i2c_error_counts_delta_during_window": error_delta,
            "i2c_error_counts_acc_gyro_angle": error_counts,
            "i2c_error_total": bus_error_total,
            "recovery_counts_at_window_start": first_recovery_counts,
            "recovery_counts_at_window_end": recovery_counts if latest_debug else None,
            "recovery_counts_delta_during_window": recovery_delta,
            "hal_error_latest_acc_gyro_angle_probe": latest_hal_errors,
            "hal_error_observations_by_bit": hal_error_counts,
            "angle_zero_count": zero_count,
            "recovery_attempt_success_failure": recovery_counts,
            "error_streak_max": error_streak_max,
            "read_rate_hz": read_rate,
            "gravity_magnitude_median_g": gravity_median,
            "acc_raw_formal_comparisons": acc_consistent_samples,
            "acc_raw_formal_mismatches": acc_mismatch_samples,
            "imu_min": [min(v[i] for _, v in capture.imu) for i in range(3)] if capture.imu else None,
            "imu_max": [max(v[i] for _, v in capture.imu) for i in range(3)] if capture.imu else None,
            "acc_min": [min(v[i] for _, v in capture.acc) for i in range(3)] if capture.acc else None,
            "acc_max": [max(v[i] for _, v in capture.acc) for i in range(3)] if capture.acc else None,
            "gyro_dps_min": [min(x[i] for _, r in capture.debug for x in [_float_vector(r["GYRO_RAW"], 2000.0 / 32768.0)]) for i in range(3)] if debug else None,
            "gyro_dps_max": [max(x[i] for _, r in capture.debug for x in [_float_vector(r["GYRO_RAW"], 2000.0 / 32768.0)]) for i in range(3)] if debug else None,
            "telemetry_max_gaps_s": gaps,
            "stack_free_min_bytes": stack_free_min,
            "stack_guards": [row["GUARD"] for _, row in capture.stack],
            "malformed_sensor_frames": capture.malformed,
            "active_alarm": capture.active_alarm,
        },
    }


def _make_run_dir() -> Path:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    run_dir = ARTIFACT_ROOT / stamp
    run_dir.mkdir()
    return run_dir


def _write_serial_line(log: Any, direction: str, line: str) -> None:
    log.write(f"{datetime.now().astimezone().isoformat(timespec='milliseconds')} {direction} {line}\n")
    log.flush()


def _read_line(port: Any, log: Any, capture: Capture, start: float) -> tuple[str, str | None]:
    raw = port.readline()
    if not raw:
        return "", None
    line = raw.decode("utf-8", errors="replace").strip()
    _write_serial_line(log, "RX", line)
    kind = capture.add_line(line, time.monotonic() - start)
    print(f"RX {line}")
    if capture.active_alarm is not None:
        raise RealImuError(f"active alarm observed; not acknowledged: {capture.active_alarm}")
    if capture.virtual_mode_seen:
        raise RealImuError("STM32 reported VIRTUAL mode during a REAL-only capture")
    return line, kind


def _establish_real_mode(port: Any, log: Any, capture: Capture, start: float) -> None:
    if hasattr(port, "reset_input_buffer"):
        port.reset_input_buffer()
    command = "TEST:EXIT"
    _write_serial_line(log, "TX", command)
    print(f"TX {command} (single attempt; REAL only)")
    port.write((command + "\n").encode("ascii"))
    if hasattr(port, "flush"):
        port.flush()
    deadline = time.monotonic() + EXIT_ACK_TIMEOUT_S
    while time.monotonic() < deadline:
        line, _ = _read_line(port, log, capture, start)
        if REAL_ACK in line:
            return
    raise RealImuError(f"REAL mode was not confirmed within {EXIT_ACK_TIMEOUT_S:.1f}s")


def _capture_window(port: Any, log: Any, capture: Capture, duration: float,
                    start: float, label: str) -> None:
    deadline = time.monotonic() + duration
    print(f"[{label}] collecting for {duration:.1f}s; keep the board still")
    while time.monotonic() < deadline:
        _read_line(port, log, capture, start)


def _motion_check(port: Any, log: Any, capture: Capture, start: float) -> bool:
    input("静态健康测试已通过。请把板子拿稳；按回车后在 10 秒内缓慢倾斜一次（不要跌落或剧烈摇晃）...")
    first = len(capture.imu)
    _capture_window(port, log, capture, 10.0, start, "MOTION CHECK")
    samples = capture.imu[first:]
    if len(samples) < 10:
        return False
    spans = [max(values[i] for _, values in samples) - min(values[i] for _, values in samples)
             for i in range(3)]
    latest_online = [row["ONLINE"] for _, row in capture.debug[-5:]]
    result = max(spans, default=0.0) >= 8.0 and bool(latest_online) and all(latest_online)
    print(f"Motion angle spans (deg): {spans}; {'PASS' if result else 'FAIL'}")
    return result


def _report(report: dict[str, Any], out: Any = sys.stdout) -> bool:
    checks = report["checks"]
    metrics = report["metrics"]
    print("\n================================", file=out)
    print(" JY61P Real Sensor Test", file=out)
    print("================================", file=out)
    print(f"{'Mode':22} {'REAL' if checks['Mode REAL confirmed'] else 'FAIL'}", file=out)
    labels = (
        ("Bringup", "Bringup JY=1/JY_RET=0"),
        ("JY Online", "JY online ratio >= 90%"),
        ("Valid ACC Samples", "ACC valid ratio >= 90%"),
        ("Valid Angle Samples", "Angle valid ratio >= 80%"),
        ("Finite IMU", "Finite/ranged IMU"),
        ("Finite ACC", "Finite/ranged ACC"),
        ("Freshness", "Freshness <= 120 ms"),
        ("I2C Errors", "I2C errors/recovery stable"),
        ("Illegal State Values", "Legal status/mask values"),
        ("Raw vectors", "Raw vectors present"),
        ("ACC scale", "ACC raw/formal scale agrees"),
        ("Telemetry cadence", "Telemetry cadence"),
        ("Stack guard", "Stack guard/watermark"),
    )
    for label, key in labels:
        print(f"{label:22} {'PASS' if checks[key] else 'FAIL'}", file=out)
    print(f"Samples: JYDBG={metrics['debug_samples']} IMU={metrics['imu_samples']} ACC={metrics['acc_samples']}", file=out)
    print(f"Online ratio: {metrics['online_ratio']:.1%}; ACC valid: {metrics['acc_valid_ratio']:.1%}; angle valid: {metrics['angle_valid_ratio']:.1%}", file=out)
    print(f"Max age: {metrics['max_age_ms']} ms; max error streak: {metrics['error_streak_max']}", file=out)
    print(
        "I2C errors ACC/GYRO/ANGLE (window start -> end; delta): "
        f"{metrics['i2c_error_counts_at_window_start']} -> "
        f"{metrics['i2c_error_counts_at_window_end']}; "
        f"delta={metrics['i2c_error_counts_delta_during_window']}; "
        f"latest HALERR={metrics['hal_error_latest_acc_gyro_angle_probe']}; "
        f"ANGLE_ZERO={metrics['angle_zero_count']}",
        file=out,
    )
    print(
        "Recovery attempts/success/failure (window start -> end; delta): "
        f"{metrics['recovery_counts_at_window_start']} -> "
        f"{metrics['recovery_counts_at_window_end']}; "
        f"delta={metrics['recovery_counts_delta_during_window']}",
        file=out,
    )
    print(f"Raw-to-ACC comparisons: {metrics['acc_raw_formal_comparisons']} (mismatch {metrics['acc_raw_formal_mismatches']})", file=out)
    print(f"Gravity magnitude median: {metrics['gravity_magnitude_median_g']} g", file=out)
    print(f"Read rate: {metrics['read_rate_hz']} Hz; max telemetry gaps: {metrics['telemetry_max_gaps_s']}", file=out)
    print(f"Stack free minimum: {metrics['stack_free_min_bytes']} bytes; guards: {metrics['stack_guards']}", file=out)
    print("STATIC REAL JY61P HEALTH: " + ("PASS" if all(checks.values()) else "FAIL"), file=out)
    print("================================", file=out)
    return all(checks.values())


def _build_and_flash(args: argparse.Namespace, run_dir: Path) -> tuple[str, str]:
    # The existing Level 3 module owns all Keil, probe discovery, build, and
    # verified flash behavior; this script deliberately does not duplicate it.
    try:
        import test_hardware as level3
    except ImportError as error:
        raise RealImuError(f"could not load the existing Level 3 helpers: {error}") from error

    project = level3.load_project_info(PROJECT_FILE, FIRMWARE)
    build_text = "SKIPPED"
    flash_text = "SKIPPED"
    if not args.skip_build:
        keil = level3.discover_keil(args.keil)
        print(f"Keil: {keil}")
        digest = level3.build_firmware(project, keil, run_dir / "build.log")
        build_text = f"PASS sha256={digest}"
        print(f"Build PASS: {digest}")

    programmer = None
    probe = None
    if not args.skip_flash:
        programmer = level3.discover_programmer(args.programmer)
        serials, listing, _ = level3.enumerate_stlink_serials(programmer, run_dir / "flash.log")
        probe = level3.select_probe_serial(serials, args.probe_serial)
        print(f"STM32 Programmer: {programmer}; ST-Link SN: {probe}")
        (run_dir / "stlink_enumeration.txt").write_text(listing, encoding="utf-8")
        level3.flash_firmware(project, programmer, probe, run_dir / "flash.log")
        flash_text = f"PASS programmer={programmer} probe={probe}"
        print("Flash/verify/reset/run PASS")
        time.sleep(1.0)
    return build_text, flash_text


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB-TTL serial port, e.g. COM15")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--duration", type=float, default=15.0,
                        help="static capture duration, 10 to 20 seconds")
    parser.add_argument("--keil", type=Path)
    parser.add_argument("--programmer", type=Path)
    parser.add_argument("--probe-serial")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-flash", action="store_true")
    parser.add_argument("--motion-check", action="store_true",
                        help="after static PASS, prompt for a slow manual tilt check")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if not 10.0 <= args.duration <= 20.0:
        print("FAIL: --duration must be between 10 and 20 seconds", file=sys.stderr)
        return 2
    if args.baud <= 0:
        print("FAIL: --baud must be positive", file=sys.stderr)
        return 2
    if args.skip_build and not args.skip_flash:
        print("FAIL: --skip-build requires --skip-flash to prevent flashing a stale HEX", file=sys.stderr)
        return 2

    run_dir = _make_run_dir()
    serial_path = run_dir / "serial.log"
    capture = Capture()
    start = time.monotonic()
    real_acknowledged = False
    build_result, flash_result = "NOT RUN", "NOT RUN"
    motion_result = "NOT RUN"
    try:
        try:
            import serial
            import test_hardware as level3
        except ImportError as error:
            raise RealImuError(f"required modules unavailable: {error}") from error
        level3.validate_serial_port(args.port, level3.list_serial_ports())
        if not args.skip_build or not args.skip_flash:
            build_result, flash_result = _build_and_flash(args, run_dir)
        else:
            print("Build/flash skipped by explicit request; using the firmware already on target.")

        with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=1.0) as port, \
             serial_path.open("w", encoding="utf-8", newline="") as log:
            _establish_real_mode(port, log, capture, start)
            real_acknowledged = True
            print("REAL mode confirmed. No virtual sensor commands will be sent.")
            _capture_window(port, log, capture, args.duration, start, "STATIC HEALTH")

        report = assess(capture, args.duration, real_acknowledged)
        static_pass = _report(report)
        if static_pass and args.motion_check:
            # Dynamic validation is deliberately opt-in and requires explicit
            # human movement; it is never part of the default static run.
            with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=1.0) as port, \
                 serial_path.open("a", encoding="utf-8", newline="") as log:
                motion_pass = _motion_check(port, log, capture, start)
            motion_result = "PASS" if motion_pass else "FAIL"
            report["checks"]["Optional motion check"] = motion_pass
        else:
            report["checks"]["Optional motion check"] = None
            motion_result = "NOT RUN"

        report["metrics"]["motion_check"] = motion_result
        report["metrics"]["build"] = build_result
        report["metrics"]["flash"] = flash_result
        report["metrics"]["git_commit"] = __import__("subprocess").run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True,
            check=False,
        ).stdout.strip()
        final_pass = all(value is not False for value in report["checks"].values())
        (run_dir / "summary.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        summary_lines = [
            f"Run directory: {run_dir}", f"Build: {build_result}", f"Flash: {flash_result}",
            f"Motion check: {motion_result}",
            "REAL JY61P HEALTH: " + ("PASS" if final_pass else "FAIL"),
        ]
        (run_dir / "summary.txt").write_text("\n".join(summary_lines) + "\n", encoding="utf-8")
        print("REAL JY61P HEALTH: " + ("PASS" if final_pass else "FAIL"))
        print(f"Artifacts: {run_dir}")
        return 0 if final_pass else 1
    except KeyboardInterrupt:
        print("FAIL: interrupted; no alarm acknowledgement or virtual sensor command was sent.", file=sys.stderr)
        return 130
    except Exception as error:
        failure = f"{type(error).__name__}: {error}"
        print(f"FAIL: {failure}\nArtifacts: {run_dir}", file=sys.stderr)
        (run_dir / "failure.txt").write_text(failure + "\n", encoding="utf-8")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
