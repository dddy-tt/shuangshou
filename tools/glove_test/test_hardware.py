#!/usr/bin/env python3
"""Level 3 local orchestrator for the Smart Glove hardware test."""
from __future__ import annotations
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Sequence
PROJECT_RELATIVE = Path("firmware/stm32f407/MDK-ARM/shuangshou.uvprojx")
FIRMWARE_RELATIVE = Path("firmware/stm32f407")
RUNNER_RELATIVE = Path("tools/glove_test/run_virtual_sensor_test.py")
CASES_RELATIVE = Path("tests/cases")
BUILD_FOOTER = re.compile(
    r"\b(?P<errors>\d+)\s+Error\(s\)\s*,\s*"
    r"(?P<warnings>\d+)\s+Warning\(s\)", re.I
)
STACK = re.compile(
    r"\[STACK\]\|SIZE=(?P<size>\d+)\|USED=(?P<used>\d+)\|"
    r"FREE=(?P<free>\d+)\|GUARD=(?P<guard>\d+)", re.I
)
C_TESTS = (
    ("alarm_engine_test", ("Core/Src/alarm.c", "tests/alarm_engine_test.c"), ("-lm",)),
    ("bluetooth_tx_test", ("tests/bluetooth_tx_test.c",), ()),
    ("jy61p_zero_filter_test", ("tests/jy61p_zero_filter_test.c",), ("-lm",)),
    ("test_input_test", ("Core/Src/test_input.c", "tests/test_input_test.c"), ("-lm",)),
    ("usart3_rx_test", ("tests/usart3_rx_test.c",), ("-lm",)),
)
WIRING_TESTS = ("alarm_wiring_test.ps1", "test_input_wiring_test.ps1", "usart3_rx_wiring_test.ps1")
class HardwareTestError(RuntimeError):
    pass
@dataclass(frozen=True)
class ProjectInfo:
    project_file: Path
    mdk_dir: Path
    firmware_root: Path
    device: str
    hex_path: Path
@dataclass(frozen=True)
class ToolPaths:
    keil: Path | None = None
    programmer: Path | None = None
    gcc: Path | None = None
    powershell: Path | None = None
@dataclass(frozen=True)
class HexSnapshot:
    exists: bool
    mtime_ns: int | None
    sha256: str | None
@dataclass(frozen=True)
class StackWatermark:
    size: int
    used: int
    free: int
    guard: int
@dataclass(frozen=True)
class PreflightInfo:
    project: ProjectInfo
    runner: Path
    cases: tuple[Path, ...]
    tools: ToolPaths
    probe_serial: str | None
    probe_listing: str
def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]
def _text(value: Any) -> str:
    if value is None:
        return ""
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else str(value)
def _rc(result: Any) -> int:
    return int(getattr(result, "returncode", 1))
def _command_text(command: Sequence[str]) -> str:
    return subprocess.list2cmdline([str(item) for item in command])
def _invoke(command: Sequence[str], cwd: Path, timeout: float | None = None) -> Any:
    options: dict[str, Any] = {
        "cwd": str(cwd), "stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
        "text": True, "errors": "replace", "check": False,
    }
    if timeout is not None:
        options["timeout"] = timeout
    try:
        return subprocess.run([str(item) for item in command], **options)
    except subprocess.TimeoutExpired as error:
        raise HardwareTestError(
            f"command timed out after {timeout}s: {_command_text(command)}"
        ) from error
    except OSError as error:
        raise HardwareTestError(f"could not start {_command_text(command)}: {error}") from error
def _log_command(
    path: Path, label: str, command: Sequence[str], cwd: Path,
    result: Any | None = None, error: BaseException | None = None,
) -> None:
    with path.open("a", encoding="utf-8", newline="") as log:
        log.write(f"\n===== {label} =====\nCWD: {cwd}\nCOMMAND: {_command_text(command)}\n")
        if result is not None:
            stdout, stderr = _text(getattr(result, "stdout", "")), _text(getattr(result, "stderr", ""))
            log.write(f"RETURN CODE: {_rc(result)}\n--- STDOUT ---\n{stdout}")
            if not stdout.endswith("\n"):
                log.write("\n")
            log.write(f"--- STDERR ---\n{stderr}")
            if not stderr.endswith("\n"):
                log.write("\n")
        if error is not None:
            log.write(f"EXCEPTION: {type(error).__name__}: {error}\n")
def _append_skip(path: Path, reason: str) -> None:
    with path.open("a", encoding="utf-8") as log:
        log.write(f"SKIPPED: {reason}\n")
def _logs(run_dir: Path) -> dict[str, Path]:
    run_dir.mkdir(parents=True, exist_ok=False)
    paths = {name: run_dir / filename for name, filename in (
        ("build", "build.log"), ("flash", "flash.log"),
        ("hardware", "hardware.log"), ("summary", "summary.txt"),
        ("software", "software_tests.log"),
    )}
    for name, path in paths.items():
        path.write_text(f"Level 3 {name} log\n", encoding="utf-8")
    return paths
def create_run_directory(repo_root: Path) -> tuple[Path, dict[str, Path]]:
    root = repo_root / "artifacts" / "hardware_test"
    root.mkdir(parents=True, exist_ok=True)
    stem = f"{datetime.now():%Y%m%d_%H%M%S_%f}_{os.getpid()}"
    run_dir = root / stem
    suffix = 1
    while run_dir.exists():
        run_dir = root / f"{stem}_{suffix}"
        suffix += 1
    return run_dir, _logs(run_dir)
def _find_candidate(value: str | Path) -> Path | None:
    path = Path(value).expanduser()
    if path.is_file():
        return path.resolve()
    found = shutil.which(str(value))
    return Path(found).resolve() if found else None
def _program_files(relative: str) -> list[Path]:
    roots = [Path(value) for key in ("ProgramFiles", "ProgramFiles(x86)")
             if (value := os.environ.get(key))]
    roots += [Path("C:/Program Files"), Path("C:/Program Files (x86)")]
    return [root / relative for root in roots]
def discover_tool(
    label: str, explicit: str | Path | None, env_name: str | None,
    path_names: Sequence[str], common: Sequence[Path],
) -> Path:
    if explicit is not None:
        found = _find_candidate(explicit)
        if found is None:
            raise HardwareTestError(f"{label} override was not found: {explicit}")
        return found
    if env_name and os.environ.get(env_name):
        value = os.environ[env_name]
        found = _find_candidate(value)
        if found is None:
            raise HardwareTestError(f"{env_name} does not point to {label}: {value}")
        return found
    for name in path_names:
        if found := shutil.which(name):
            return Path(found).resolve()
    for path in common:
        if path.is_file():
            return path.resolve()
    raise HardwareTestError(f"{label} was not found on PATH or common locations")
def discover_keil(explicit: str | Path | None = None) -> Path:
    common = [
        Path("E:/Keil5/Local/Keil_v5/UV4/UV4.exe"), Path("E:/Keil5/Keil_v5/UV4/UV4.exe"),
        Path("C:/Keil_v5/UV4/UV4.exe"), Path("C:/Keil/UV4/UV4.exe"),
    ] + _program_files("Keil_v5/UV4/UV4.exe")
    return discover_tool("Keil", explicit, "GLOVE_KEIL", ("UV4.exe", "UV4"), common)
def discover_programmer(explicit: str | Path | None = None) -> Path:
    common = [
        Path("E:/download/bin/STM32_Programmer_CLI.exe"),
        Path("C:/ST/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"),
    ] + _program_files(
        "STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
    )
    return discover_tool(
        "STM32 Programmer", explicit, "GLOVE_STM32_PROGRAMMER",
        ("STM32_Programmer_CLI.exe", "STM32_Programmer_CLI"), common,
    )
def discover_gcc() -> Path:
    return discover_tool(
        "GCC", None, None, ("gcc.exe", "gcc"),
        [Path("E:/winlibs/mingw64/bin/gcc.exe"), Path("C:/mingw64/bin/gcc.exe"),
         Path("C:/msys64/mingw64/bin/gcc.exe")],
    )
def discover_powershell() -> Path:
    return discover_tool(
        "PowerShell", None, None,
        ("powershell.exe", "powershell", "pwsh.exe", "pwsh"),
        [Path("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"),
         Path("C:/Program Files/PowerShell/7/pwsh.exe")],
    )
def _port_device(port: Any) -> str:
    return str(getattr(port, "device", port))
def _port_info(port: Any) -> str:
    return " ".join(str(getattr(port, key, "")) for key in
                     ("description", "manufacturer", "product", "hwid")).upper()
def list_serial_ports() -> list[Any]:
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise HardwareTestError(
            "pyserial is required: python -m pip install -r tools/glove_test/requirements.txt"
        ) from error
    return list(list_ports.comports())
def validate_serial_port(requested: str, ports: Iterable[Any]) -> None:
    wanted, ports = requested.strip().upper(), list(ports)
    for port in ports:
        if _port_device(port).strip().upper() != wanted:
            continue
        info = _port_info(port)
        if ("ST-LINK" in info or "STLINK" in info) and ("VCP" in info or "VIRTUAL COM" in info):
            raise HardwareTestError(f"{requested} is the ST-Link VCP; use USB-TTL COM15/CH340.")
        return
    raise HardwareTestError(
        f"serial port {requested} was not found; detected ports: "
        f"{[_port_device(port) for port in ports] or 'none'}"
    )
def select_cases(repo_root: Path, selected: Any, all_cases: bool = False) -> tuple[Path, ...]:
    if selected and all_cases:
        raise HardwareTestError("--case and --all-cases cannot be used together")
    if selected:
        values = [selected] if isinstance(selected, (str, Path)) else list(selected)
        cases = tuple((repo_root / Path(value)).resolve() if not Path(value).is_absolute()
                      else Path(value).resolve() for value in values)
    else:
        cases = tuple(sorted((repo_root / CASES_RELATIVE).glob("*.json"), key=lambda path: path.name))
    if not cases:
        raise HardwareTestError("no JSON cases were found")
    for case in cases:
        if not case.is_file() or case.suffix.lower() != ".json":
            raise HardwareTestError(f"invalid or missing JSON case: {case}")
    return cases
def load_project_info(project_file: Path, firmware_root: Path) -> ProjectInfo:
    project_file, firmware_root = project_file.resolve(), firmware_root.resolve()
    if not project_file.is_file():
        raise HardwareTestError(f"Keil project was not found: {project_file}")
    try:
        root = ET.parse(project_file).getroot()
    except (ET.ParseError, OSError) as error:
        raise HardwareTestError(f"could not read Keil project XML: {error}") from error
    targets = root.findall(".//Target")
    target = next((item for item in targets if item.findtext("TargetName") == "shuangshou"),
                  targets[0] if targets else None)
    if target is None:
        raise HardwareTestError("Keil project has no target")
    common = target.find("TargetOption/TargetCommonOption")
    device = common.findtext("Device", "").strip() if common is not None else ""
    if device != "STM32F407ZGTx":
        raise HardwareTestError(f"Keil project device must be STM32F407ZGTx, found {device or 'missing'}")
    if common is None or (common.findtext("CreateHexFile") or "").strip() != "1":
        raise HardwareTestError("Keil project is not configured to create a HEX file")
    output_dir, output_name = (common.findtext("OutputDirectory") or "").strip(), (common.findtext("OutputName") or "").strip()
    if not output_dir or not output_name:
        raise HardwareTestError("Keil project does not define an output directory/name")
    hex_path = (project_file.parent / Path(output_dir.replace("\\", "/")) / f"{output_name}.hex").resolve()
    try:
        hex_path.relative_to(firmware_root)
    except ValueError as error:
        raise HardwareTestError(f"project HEX output escapes the firmware tree: {hex_path}") from error
    return ProjectInfo(project_file, project_file.parent, firmware_root, device, hex_path)
def _serial_value(line: str) -> str | None:
    tail = line.split(":", 1)[-1] if ":" in line else line.split("=", 1)[-1]
    match = re.search(r"\b[A-Z0-9][A-Z0-9_-]{7,31}\b", tail, re.I)
    return match.group(0) if match else None
def parse_stlink_serials(output: str) -> list[str]:
    serials, seen, section = [], set(), None
    for raw in output.splitlines():
        line, lower = raw.strip(), raw.lower()
        vcp = any(marker in lower for marker in ("vcp", "virtual com", "com port", "serial port"))
        stlink = bool(re.search(r"st[ -]?link", lower))
        if vcp:
            section = "vcp"
        elif stlink:
            section = "stlink"
        explicit = bool(re.search(r"st[ -]?link[^:]*\b(?:sn|serial(?: number)?)\b", lower))
        generic = bool(re.search(r"\b(?:sn|serial(?: number)?)\s*[:=]", lower))
        if (explicit and not vcp) or (section == "stlink" and generic and not vcp):
            if value := _serial_value(line):
                key = value.upper()
                if key not in seen:
                    seen.add(key)
                    serials.append(value)
    return serials
def enumerate_stlink_serials(programmer: Path, log_path: Path | None = None) -> tuple[list[str], str, int]:
    command = [programmer, "-l"]
    try:
        result = _invoke(command, programmer.parent)
    except BaseException as error:
        if log_path is not None and not isinstance(error, (KeyboardInterrupt, SystemExit)):
            _log_command(log_path, "ST-Link enumeration", command, programmer.parent, error=error)
        raise
    if log_path is not None:
        _log_command(log_path, "ST-Link enumeration", command, programmer.parent, result=result)
    output, error = _text(getattr(result, "stdout", "")), _text(getattr(result, "stderr", ""))
    combined = output + (("\n" if output else "") + error if error else "")
    if _rc(result) != 0:
        raise HardwareTestError(f"ST-Link enumeration failed with return code {_rc(result)}")
    return parse_stlink_serials(combined), f"RETURN CODE: {_rc(result)}\n{combined}", _rc(result)
def select_probe_serial(serials: Iterable[str], requested: str | None = None) -> str:
    unique, seen = [], set()
    for serial in serials:
        value = str(serial).strip()
        if value and value.upper() not in seen:
            seen.add(value.upper())
            unique.append(value)
    if requested:
        for serial in unique:
            if serial.upper() == requested.strip().upper():
                return serial
        raise HardwareTestError(f"requested ST-Link serial {requested} was not enumerated: {unique or 'none'}")
    if len(unique) != 1:
        raise HardwareTestError("no ST-Link probe was enumerated" if not unique else
                                f"multiple ST-Link probes were enumerated; pass --probe-serial: {unique}")
    return unique[0]
def preflight(args: argparse.Namespace, repo_root: Path, hardware_log: Path | None = None) -> PreflightInfo:
    firmware = (repo_root / FIRMWARE_RELATIVE).resolve()
    project = load_project_info((repo_root / PROJECT_RELATIVE).resolve(), firmware)
    runner = (repo_root / RUNNER_RELATIVE).resolve()
    if not runner.is_file():
        raise HardwareTestError(f"Level 2 runner was not found: {runner}")
    cases = select_cases(repo_root, args.case, args.all_cases)
    validate_serial_port(args.port, list_serial_ports())
    tools = ToolPaths(
        keil=None if args.skip_build else discover_keil(args.keil),
        gcc=None if args.skip_software_tests else discover_gcc(),
        powershell=None if args.skip_software_tests else discover_powershell(),
    )
    listing, serial = "ST-Link listing skipped", args.probe_serial if args.skip_flash else None
    if hardware_log is not None and args.skip_flash:
        _append_skip(hardware_log, "ST-Link enumeration (--skip-flash)")
    if not args.skip_flash:
        programmer = discover_programmer(args.programmer)
        serials, listing, _ = enumerate_stlink_serials(programmer, hardware_log)
        serial = select_probe_serial(serials, args.probe_serial)
        tools = ToolPaths(tools.keil, programmer, tools.gcc, tools.powershell)
    return PreflightInfo(project, runner, cases, tools, serial, listing)
def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
def snapshot_hex(path: Path) -> HexSnapshot:
    if not path.is_file():
        return HexSnapshot(False, None, None)
    return HexSnapshot(True, path.stat().st_mtime_ns, sha256_file(path))
def _wait_for_build_footer(path: Path, timeout: float = 30.0, poll_seconds: float = 0.1) -> str:
    deadline = time.monotonic() + timeout
    while True:
        if path.is_file():
            text = path.read_text(encoding="utf-8", errors="replace")
            if BUILD_FOOTER.search(text):
                return text
        if time.monotonic() >= deadline:
            raise HardwareTestError(f"Keil log did not reach its complete footer within {timeout}s: {path}")
        time.sleep(min(poll_seconds, max(0.0, deadline - time.monotonic())))
def _require_fresh(before: HexSnapshot, after: HexSnapshot, started_ns: int, path: Path) -> None:
    if not after.exists or after.mtime_ns is None or after.sha256 is None:
        raise HardwareTestError(f"Keil did not produce the expected HEX: {path}")
    if before.exists and before.mtime_ns == after.mtime_ns and before.sha256 == after.sha256:
        raise HardwareTestError(f"HEX was stale after this Build invocation: {path}")
    if after.mtime_ns <= started_ns:
        raise HardwareTestError(f"HEX mtime is not newer than this Build invocation: {path}")
def build_firmware(project: ProjectInfo, keil: Path, build_log: Path, footer_timeout: float = 30.0) -> str:
    before, started = snapshot_hex(project.hex_path), time.time_ns()
    command = [keil, "-r", project.project_file, "-o", build_log, "-j0"]
    try:
        result = _invoke(command, project.mdk_dir, timeout=600.0)
    except BaseException as error:
        if not isinstance(error, (KeyboardInterrupt, SystemExit)):
            _log_command(build_log, "Keil process", command, project.mdk_dir, error=error)
        raise
    try:
        if _rc(result) != 0:
            raise HardwareTestError(f"Keil returned nonzero exit code {_rc(result)}")
        footer = _wait_for_build_footer(build_log, footer_timeout)
        match = list(BUILD_FOOTER.finditer(footer))[-1]
        if match.group("errors") != "0" or match.group("warnings") != "0":
            raise HardwareTestError(
                f"Keil build did not meet the zero-error/zero-warning gate: "
                f"{match.group('errors')} Error(s), {match.group('warnings')} Warning(s)"
            )
        after = snapshot_hex(project.hex_path)
        _require_fresh(before, after, started, project.hex_path)
        assert after.sha256 is not None
        return after.sha256
    finally:
        _log_command(build_log, "Keil process", command, project.mdk_dir, result=result)
def _flash_success(output: str) -> tuple[bool, bool, bool]:
    verify = bool(re.search(r"download\s+verified\s+successfully|verification[^\r\n]*success|verify[^\r\n]*success", output, re.I))
    reset = bool(re.search(r"\bMCU\s+Reset\b|\breset[^\r\n]*(?:success|complete|done)", output, re.I))
    run = bool(re.search(r"\bCore\s+run\b|\brun[^\r\n]*(?:success|complete|done|running)", output, re.I))
    return verify, reset, run
def flash_firmware(project: ProjectInfo, programmer: Path, serial: str, flash_log: Path) -> None:
    if not project.hex_path.is_file():
        raise HardwareTestError(f"HEX to flash does not exist: {project.hex_path}")
    command = [programmer, "-c", "port=SWD", f"sn={serial}", "mode=UR", "freq=1000",
               "-d", project.hex_path, "-v", "-rst", "-run"]
    try:
        result = _invoke(command, project.firmware_root, timeout=180.0)
    except BaseException as error:
        if not isinstance(error, (KeyboardInterrupt, SystemExit)):
            _log_command(flash_log, "STM32 Programmer flash", command, project.firmware_root, error=error)
        raise
    try:
        if _rc(result) != 0:
            raise HardwareTestError(f"STM32 Programmer flash returned nonzero exit code {_rc(result)}")
        output = _text(getattr(result, "stdout", "")) + "\n" + _text(getattr(result, "stderr", ""))
        verify, reset, run = _flash_success(output)
        if not all((verify, reset, run)):
            missing = ", ".join(name for name, ok in (("verify", verify), ("reset", reset), ("run", run)) if not ok)
            raise HardwareTestError(f"flash output did not confirm successful {missing}")
    finally:
        _log_command(flash_log, "STM32 Programmer flash", command, project.firmware_root, result=result)
def _run_logged(log: Path, label: str, command: Sequence[str], cwd: Path, timeout: float = 300.0) -> Any:
    try:
        result = _invoke(command, cwd, timeout)
    except BaseException as error:
        if not isinstance(error, (KeyboardInterrupt, SystemExit)):
            _log_command(log, label, command, cwd, error=error)
        raise
    _log_command(log, label, command, cwd, result=result)
    if _rc(result) != 0:
        raise HardwareTestError(f"{label} failed with return code {_rc(result)}")
    return result
def run_software_tests(repo_root: Path, project: ProjectInfo, tools: ToolPaths, software_log: Path) -> None:
    if tools.gcc is None or tools.powershell is None:
        raise HardwareTestError("software-test tools were not discovered")
    include, firmware = project.firmware_root / "Core/Inc", project.firmware_root
    for name, sources, flags in C_TESTS:
        exe = software_log.parent / f"{name}.exe"
        command = [tools.gcc, "-std=c99", "-Wall", "-Wextra", "-Werror", f"-I{include}"]
        command += [firmware / source for source in sources] + list(flags) + ["-o", exe]
        _run_logged(software_log, f"compile {name}", command, firmware)
        if not exe.is_file():
            raise HardwareTestError(f"GCC reported success but did not create {exe}")
        _run_logged(software_log, f"run {name}", [exe], firmware)
    _run_logged(
        software_log, "Python unittest discovery",
        [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py", "-v"],
        repo_root,
    )
    tests_dir = firmware / "tests"
    for script_name in WIRING_TESTS:
        _run_logged(
            software_log, f"wiring {script_name}",
            [tools.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", tests_dir / script_name],
            firmware,
        )
def parse_stack_watermarks(output: str) -> list[StackWatermark]:
    return [StackWatermark(*(int(match.group(key)) for key in ("size", "used", "free", "guard")))
            for match in STACK.finditer(output)]
def run_hardware_cases(repo_root: Path, runner: Path, cases: Sequence[Path], port: str, baud: int, hardware_log: Path, case_results: list[str] | None = None) -> list[StackWatermark]:
    watermarks, failures = [], []
    def report(case: Path, passed: bool, reason: str = "") -> None:
        line = f"CASE {case.name} {'PASS' if passed else 'FAIL'}" + (f": {reason}" if reason else "")
        (case_results if case_results is not None else []).append(line)
        with hardware_log.open("a", encoding="utf-8") as log:
            log.write(line + "\n")
        print(f"[L3] {line}", file=None if passed else sys.stderr)
    for case in cases:
        print(f"[L3] Runner case: {case.name}")
        command = [sys.executable, runner, "--port", port, "--baud", str(baud), "--case", case]
        try:
            result = _invoke(command, repo_root, timeout=120.0)
        except BaseException as error:
            if not isinstance(error, (KeyboardInterrupt, SystemExit)):
                _log_command(hardware_log, f"Runner {case.name}", command, repo_root, error=error)
            report(case, False, "Runner execution was not confirmed")
            raise HardwareTestError(f"{case.name}: Runner execution was not confirmed: {error}") from error
        _log_command(hardware_log, f"Runner {case.name}", command, repo_root, result=result)
        output = _text(getattr(result, "stdout", "")) + "\n" + _text(getattr(result, "stderr", ""))
        current = parse_stack_watermarks(output)
        watermarks += current
        if not re.search(r"\[TEST\]\s+MODE=REAL\b", output):
            report(case, False, "TEST:EXIT confirmation missing")
            raise HardwareTestError(f"{case.name}: TEST:EXIT confirmation missing; stopping safely")
        if any(item.guard == 0 for item in current):
            report(case, False, "[STACK] GUARD=0")
            raise HardwareTestError(f"{case.name}: [STACK] GUARD=0; stopping")
        if _rc(result) != 0:
            reason = f"runner return code {_rc(result)}"
            failures.append(f"{case.name}: {reason}")
            report(case, False, reason)
        elif not re.search(r"\bPASS:\s*", output):
            reason = "runner PASS marker missing"
            failures.append(f"{case.name}: {reason}")
            report(case, False, reason)
        else:
            report(case, True)
    if failures:
        raise HardwareTestError("hardware cases failed: " + "; ".join(failures))
    return watermarks
def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--case", action="append", type=Path)
    parser.add_argument("--all-cases", action="store_true")
    parser.add_argument("--keil", type=Path)
    parser.add_argument("--programmer", type=Path)
    parser.add_argument("--probe-serial")
    parser.add_argument("--skip-software-tests", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-flash", action="store_true")
    return parser
def _git_sha(repo_root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=str(repo_root), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, errors="replace", check=False, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    value = _text(getattr(result, "stdout", "")).strip()
    return value if _rc(result) == 0 and value else None
def run_workflow(args: argparse.Namespace, repo_root: Path) -> tuple[int, Path]:
    run_dir, logs = create_run_directory(repo_root)
    started = datetime.now().astimezone().isoformat(timespec="seconds")
    case_results: list[str] = []
    summary = [
        "Smart Glove Level 3 hardware test", f"Repository: {repo_root}",
        f"Run directory: {run_dir}", f"Start: {started}",
        f"Port: {args.port} @ {args.baud}", "Status: RUNNING",
    ]
    if sha := _git_sha(repo_root):
        summary.append(f"Git SHA: {sha}")
    status = 1
    try:
        if args.baud <= 0:
            raise HardwareTestError("--baud must be positive")
        if args.skip_build and not args.skip_flash:
            raise HardwareTestError("--skip-build requires --skip-flash")
        print("[L3] Preflight")
        info = preflight(args, repo_root, logs["hardware"])
        summary += ["Preflight: PASS", f"Project: {info.project.project_file}",
                     f"Device: {info.project.device}", f"HEX: {info.project.hex_path}",
                     f"Cases: {', '.join(path.name for path in info.cases)}"]
        if info.probe_serial:
            summary.append(f"ST-Link serial: {info.probe_serial}")
        if args.skip_software_tests:
            _append_skip(logs["software"], "--skip-software-tests")
            summary.append("Software tests: SKIPPED")
        else:
            print("[L3] Software regression")
            run_software_tests(repo_root, info.project, info.tools, logs["software"])
            summary.append("Software tests: PASS")
        if args.skip_build:
            _append_skip(logs["build"], "--skip-build")
            summary.append("Build: SKIPPED")
        else:
            print("[L3] Keil Rebuild All")
            assert info.tools.keil is not None
            digest = build_firmware(info.project, info.tools.keil, logs["build"])
            print("[L3] Build PASS: 0 Error(s), 0 Warning(s)")
            summary.append(f"Build: PASS; 0 Error(s), 0 Warning(s); HEX SHA-256={digest}")
        if args.skip_flash:
            _append_skip(logs["flash"], "--skip-flash")
            summary.append("Flash: SKIPPED")
        else:
            print("[L3] STM32 Programmer flash")
            assert info.tools.programmer is not None and info.probe_serial is not None
            flash_firmware(info.project, info.tools.programmer, info.probe_serial, logs["flash"])
            summary.append("Flash: PASS (verify/reset/run confirmed)")
            time.sleep(0.8)
        print("[L3] Hardware cases")
        watermarks = run_hardware_cases(repo_root, info.runner, info.cases, args.port, args.baud, logs["hardware"], case_results)
        summary += ([f"STACK: SIZE={item.size} USED={item.used} FREE={item.free} GUARD={item.guard}" for item in watermarks]
                    or ["STACK: watermark unavailable (informational; no minimum FREE threshold)"])
        summary.append("Hardware cases: PASS")
        status = 0
    except BaseException as error:
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        summary.append(f"Failure: {type(error).__name__}: {error}")
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        summary[5] = "Status: PASS" if status == 0 else "Status: FAIL"
        summary += case_results
        summary.append(f"End: {datetime.now().astimezone().isoformat(timespec='seconds')}")
        overall = "LEVEL 3 PASS" if status == 0 else "LEVEL 3 FAIL"
        summary.append(overall)
        logs["summary"].write_text("\n".join(summary) + "\n", encoding="utf-8")
        print(overall, file=None if status == 0 else sys.stderr)
        print(f"Artifacts: {run_dir}")
    return status, run_dir
def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.skip_build and not args.skip_flash:
        parser.error("--skip-build requires --skip-flash")
    try:
        return run_workflow(args, _repo_root())[0]
    except KeyboardInterrupt:
        print("FAIL: interrupted", file=sys.stderr)
        return 130
if __name__ == "__main__":
    raise SystemExit(main())
