#!/usr/bin/env python3
"""Inject one virtual glove case through STM32 USART3 and verify telemetry."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from pathlib import Path
from typing import Any


FLEX_KEYS = ["L1", "L2", "L3", "L4", "L5", "R1", "R2", "R3", "R4", "R5"]
IMU_KEYS = ["roll", "pitch", "yaw"]
ACC_KEYS = ["x", "y", "z"]
TELEMETRY_PATTERNS = {
    "FLEX": re.compile(
        r"FLEX\|L1=(\d+)\|L2=(\d+)\|L3=(\d+)\|L4=(\d+)\|L5=(\d+)"
        r"\|R1=(\d+)\|R2=(\d+)\|R3=(\d+)\|R4=(\d+)\|R5=(\d+)"
    ),
    "IMU": re.compile(
        r"IMU\|R=([-+]?\d+(?:\.\d+)?)\|P=([-+]?\d+(?:\.\d+)?)"
        r"\|Y=([-+]?\d+(?:\.\d+)?)"
    ),
    "ACC": re.compile(
        r"ACC\|X=([-+]?\d+(?:\.\d+)?)\|Y=([-+]?\d+(?:\.\d+)?)"
        r"\|Z=([-+]?\d+(?:\.\d+)?)\|VALID=([01])"
    ),
}


class TestFailure(RuntimeError):
    pass


def _finite_number(value: Any, label: str, limit: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be a number")
    value = float(value)
    if not math.isfinite(value) or not -limit <= value <= limit:
        raise ValueError(f"{label} must be finite and within {-limit}..{limit}")
    return value


def validate_case(data: dict[str, Any]) -> dict[str, Any]:
    flex = data.get("flex")
    if not isinstance(flex, list) or len(flex) != 10:
        raise ValueError("flex must contain exactly 10 values")
    normalized_flex: list[int] = []
    for index, value in enumerate(flex):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(f"flex[{index}] must be an integer from 0 to 100")
        if int(value) != value or not 0 <= int(value) <= 100:
            raise ValueError(f"flex[{index}] must be an integer from 0 to 100")
        normalized_flex.append(int(value))

    imu = data.get("imu")
    acc = data.get("acc")
    if not isinstance(imu, dict) or not isinstance(acc, dict):
        raise ValueError("imu and acc must be objects")
    normalized_imu = {
        key: _finite_number(imu.get(key), f"imu.{key}", 180.0)
        for key in IMU_KEYS
    }
    normalized_acc = {
        key: _finite_number(acc.get(key), f"acc.{key}", 16.0)
        for key in ACC_KEYS
    }
    valid = acc.get("valid")
    if not isinstance(valid, bool):
        raise ValueError("acc.valid must be true or false")
    normalized_acc["valid"] = valid

    return {
        "name": str(data.get("name") or "unnamed"),
        "flex": normalized_flex,
        "imu": normalized_imu,
        "acc": normalized_acc,
    }


def load_case(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return validate_case(json.load(handle))


def build_commands(case: dict[str, Any]) -> list[tuple[str, str]]:
    flex_fields = "|".join(
        f"{key}={value}" for key, value in zip(FLEX_KEYS, case["flex"])
    )
    imu = case["imu"]
    acc = case["acc"]
    return [
        ("TEST:ENTER", "[TEST] MODE=VIRTUAL"),
        (f"TEST:FLEX|{flex_fields}", "[TEST] FLEX=OK"),
        (
            f"TEST:IMU|R={imu['roll']:.2f}|P={imu['pitch']:.2f}|Y={imu['yaw']:.2f}",
            "[TEST] IMU=OK",
        ),
        (
            f"TEST:ACC|X={acc['x']:.3f}|Y={acc['y']:.3f}|Z={acc['z']:.3f}|"
            f"VALID={1 if acc['valid'] else 0}",
            "[TEST] ACC=OK",
        ),
        ("TEST:APPLY", "[TEST] APPLY=OK"),
    ]


def _read_text_line(port: Any) -> str:
    raw = port.readline()
    if not raw:
        return ""
    return raw.decode("ascii", errors="ignore").strip()


def _wait_for(port: Any, expected: str, timeout: float) -> list[str]:
    deadline = time.monotonic() + timeout
    seen: list[str] = []
    while time.monotonic() < deadline:
        line = _read_text_line(port)
        if not line:
            continue
        seen.append(line)
        print(f"RX {line}")
        if expected in line:
            return seen
        if "[TEST] ERROR=" in line:
            raise TestFailure(f"STM32 rejected command: {line}")
    raise TestFailure(f"timeout waiting for {expected}; last={seen[-5:]}")


def _send_with_retries(
    port: Any,
    command: str,
    expected: str,
    timeout: float,
    attempts: int,
    label: str,
) -> list[str]:
    if attempts < 1:
        raise ValueError(f"{label} attempts must be at least 1")
    for attempt in range(1, attempts + 1):
        print(f"TX {command}")
        port.write((command + "\n").encode("ascii"))
        if hasattr(port, "flush"):
            port.flush()
        try:
            return _wait_for(port, expected, timeout)
        except TestFailure as error:
            if not str(error).startswith("timeout waiting for ") or attempt == attempts:
                raise
            print(
                f"WARN: {label} acknowledgement timed out; "
                f"retrying ({attempt + 1}/{attempts})",
                file=sys.stderr,
            )
    raise AssertionError("retry loop must return or raise")


def _update_latest_telemetry(line: str, latest: dict[str, Any]) -> bool:
    for kind, pattern in TELEMETRY_PATTERNS.items():
        match = pattern.fullmatch(line)
        if match is None:
            continue
        values = match.groups()
        if kind == "FLEX":
            latest[kind] = [int(value) for value in values]
        elif kind == "IMU":
            latest[kind] = [float(value) for value in values]
        else:
            latest[kind] = [float(value) for value in values[:3]] + [values[3] == "1"]
        return True
    return False


def _telemetry_matches(case: dict[str, Any], latest: dict[str, Any]) -> bool:
    flex, imu, acc = (latest[kind] for kind in ("FLEX", "IMU", "ACC"))
    if flex != case["flex"] or imu is None or acc is None:
        return False
    expected_imu = [case["imu"][key] for key in IMU_KEYS]
    expected_acc = [case["acc"][key] for key in ACC_KEYS]
    return (
        all(abs(actual - expected) <= 0.02 for actual, expected in zip(imu, expected_imu))
        and all(abs(actual - expected) <= 0.005 for actual, expected in zip(acc[:3], expected_acc))
        and acc[3] == case["acc"]["valid"]
    )


def _telemetry_timeout_error(case: dict[str, Any], latest: dict[str, Any]) -> TestFailure:
    return TestFailure(
        "telemetry did not converge within timeout:\n"
        f"  expected FLEX={case['flex']}\n  latest FLEX={latest['FLEX']}\n"
        f"  expected IMU={[case['imu'][key] for key in IMU_KEYS]}\n"
        f"  latest IMU={latest['IMU']}\n"
        f"  expected ACC={[case['acc'][key] for key in ACC_KEYS] + [case['acc']['valid']]}\n"
        f"  latest ACC={latest['ACC']}"
    )


def run_virtual_test(
    port: Any,
    case: dict[str, Any],
    timeout: float = 4.0,
    settle_seconds: float = 0.75,
    enter_attempts: int = 3,
    enter_timeout: float = 1.5,
    exit_attempts: int = 3,
    exit_timeout: float = 1.0,
) -> None:
    if settle_seconds < 0:
        raise ValueError("settle_seconds must not be negative")
    if timeout <= 0 or enter_timeout <= 0 or exit_timeout <= 0:
        raise ValueError("command timeouts must be positive")
    if enter_attempts < 1 or exit_attempts < 1:
        raise ValueError("ENTER and EXIT attempts must be at least 1")

    primary_error: BaseException | None = None
    try:
        if settle_seconds:
            time.sleep(settle_seconds)
        if hasattr(port, "reset_input_buffer"):
            port.reset_input_buffer()

        for command, expected in build_commands(case):
            if command == "TEST:ENTER":
                _send_with_retries(
                    port, command, expected, enter_timeout,
                    enter_attempts, "TEST:ENTER",
                )
            else:
                print(f"TX {command}")
                port.write((command + "\n").encode("ascii"))
                if hasattr(port, "flush"):
                    port.flush()
                _wait_for(port, expected, timeout)

        deadline = time.monotonic() + timeout
        latest: dict[str, Any] = {"FLEX": None, "IMU": None, "ACC": None}
        while time.monotonic() < deadline:
            line = _read_text_line(port)
            if line:
                print(f"RX {line}")
                if _update_latest_telemetry(line, latest) and _telemetry_matches(case, latest):
                    return
        raise _telemetry_timeout_error(case, latest)
    except BaseException as error:
        primary_error = error
        raise
    finally:
        try:
            _send_with_retries(
                port, "TEST:EXIT", "[TEST] MODE=REAL", exit_timeout,
                exit_attempts, "TEST:EXIT",
            )
        except Exception as exit_error:
            if primary_error is None:
                raise TestFailure(
                    f"telemetry passed but TEST:EXIT was not confirmed: {exit_error}"
                ) from exit_error
            print(
                f"WARN: could not confirm TEST:EXIT after failure: {exit_error}",
                file=sys.stderr,
            )


def _list_ports() -> list[str]:
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("pyserial is required: python -m pip install pyserial") from error
    return [port.device for port in list_ports.comports()]


def _open_port(name: str, baud: int, timeout: float) -> Any:
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: python -m pip install pyserial") from error
    return serial.Serial(name, baudrate=baud, timeout=0.1, write_timeout=timeout)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    default_case = repo_root / "tests" / "cases" / "mixed.json"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB-TTL serial port, for example COM5")
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--case", type=Path, default=default_case)
    parser.add_argument("--timeout", type=float, default=4.0)
    parser.add_argument("--settle-seconds", type=float, default=0.75)
    parser.add_argument("--enter-timeout", type=float, default=1.5)
    parser.add_argument("--enter-attempts", type=int, default=3)
    parser.add_argument("--exit-timeout", type=float, default=1.0)
    parser.add_argument("--exit-attempts", type=int, default=3)
    parser.add_argument("--list-ports", action="store_true")
    args = parser.parse_args()

    if args.list_ports:
        ports = _list_ports()
        print("\n".join(ports) if ports else "No serial ports found")
        return 0
    if not args.port:
        ports = _list_ports()
        parser.error(f"--port is required; detected ports: {ports or 'none'}")

    try:
        case = load_case(args.case)
        port = _open_port(args.port, args.baud, args.timeout)
        try:
            run_virtual_test(
                port,
                case,
                timeout=args.timeout,
                settle_seconds=args.settle_seconds,
                enter_attempts=args.enter_attempts,
                enter_timeout=args.enter_timeout,
                exit_attempts=args.exit_attempts,
                exit_timeout=args.exit_timeout,
            )
        finally:
            port.close()
    except (OSError, ValueError, RuntimeError, TestFailure) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(f"PASS: {case['name']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
