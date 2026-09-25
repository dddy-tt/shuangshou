#!/usr/bin/env python3
"""Run Mini Program regressions, then the existing Level 3 hardware workflow."""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable


ROOT = Path(__file__).resolve().parents[1]
MINIPROGRAM_TEST_DIR = ROOT / "miniprogram" / "test"
LEVEL3_SCRIPT = ROOT / "tools" / "glove_test" / "test_hardware.py"
ARTIFACT_ROOT = ROOT / "artifacts" / "hardware_test"

FOCUSED_TEST_FILES = (
    "gesture-matcher.test.js",
    "gesture-pages.test.js",
    "gesture-store.test.js",
    "translation-page.test.js",
    "tts.test.js",
    "rehabilitation-speech.test.js",
    "closed-loop-data-flow.test.js",
    "multi-device.test.js",
    "multi-device-page.test.js",
    "multi-device-heartbeat.test.js",
    "multi-device-firmware.test.js",
    "alarm-ack.test.js",
    "alarm-audio.test.js",
    "alarm-integration.test.js",
    "alarm-pages.test.js",
    "alarm-runtime.test.js",
    "safety-monitor.test.js",
)
C_HOST_TESTS = (
    "alarm_engine_test",
    "bluetooth_tx_test",
    "jy61p_zero_filter_test",
    "test_input_test",
    "usart3_rx_test",
)
WIRING_TESTS = (
    "alarm_wiring_test.ps1",
    "test_input_wiring_test.ps1",
    "usart3_rx_wiring_test.ps1",
)
BUILD_FOOTER = re.compile(
    r"\b(?P<errors>\d+)\s+Error\(s\)\s*,\s*"
    r"(?P<warnings>\d+)\s+Warning\(s\)", re.IGNORECASE
)


@dataclass(frozen=True)
class BatchResult:
    passed_files: int
    total_files: int
    failures: tuple[str, ...] = ()

    @property
    def passed(self) -> bool:
        return self.total_files > 0 and self.passed_files == self.total_files and not self.failures


@dataclass(frozen=True)
class StageResult:
    status: str
    detail: str = ""


@dataclass(frozen=True)
class Level3Result:
    firmware_host: StageResult
    python_tests: StageResult
    wiring: StageResult
    rebuild: StageResult
    flash: StageResult
    hardware_cases: StageResult
    stack_guard: StageResult
    overall: StageResult
    artifact_dir: Path | None = None


def _not_run(detail: str = "") -> StageResult:
    return StageResult("NOT RUN", detail)


def enumerate_miniprogram_tests(test_dir: Path = MINIPROGRAM_TEST_DIR) -> tuple[Path, ...]:
    """Enumerate test files explicitly; do not pass an unexpanded wildcard to Node."""
    if not test_dir.is_dir():
        return ()
    return tuple(sorted(
        (path for path in test_dir.iterdir()
         if path.is_file() and path.name.endswith(".test.js")),
        key=lambda path: path.name.casefold(),
    ))


def focused_test_files(test_dir: Path = MINIPROGRAM_TEST_DIR) -> tuple[tuple[Path, ...], tuple[str, ...]]:
    available = {path.name: path for path in enumerate_miniprogram_tests(test_dir)}
    selected = tuple(available[name] for name in FOCUSED_TEST_FILES if name in available)
    missing = tuple(name for name in FOCUSED_TEST_FILES if name not in available)
    return selected, missing


def run_node_file_suite(
    files: Iterable[Path],
    repo_root: Path = ROOT,
    *,
    node_executable: str | None = None,
    label: str = "MiniProgram",
) -> BatchResult:
    selected = tuple(files)
    if not selected:
        return BatchResult(0, 0, ("no .test.js files were found",))
    node = node_executable or shutil.which("node")
    if not node:
        print(f"[{label}] FAIL: node executable was not found on PATH")
        return BatchResult(0, len(selected), ("node executable was not found on PATH",))

    passed = 0
    failures: list[str] = []
    for test_file in selected:
        try:
            relative = test_file.resolve().relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            failures.append(f"test path is outside repository: {test_file}")
            print(f"[{label}] FAIL: test path is outside repository: {test_file}")
            continue
        argv = [node, "--test", relative]
        try:
            result = subprocess.run(
                argv,
                cwd=str(repo_root),
                capture_output=True,
                text=True,
                errors="replace",
                check=False,
                timeout=120,
            )
            if result.returncode == 0:
                passed += 1
                print(f"[{label}] PASS: {relative}")
            else:
                failures.append(relative)
                print(f"[{label}] FAIL: {relative} (exit {result.returncode})")
                if result.stdout:
                    print(result.stdout.rstrip())
                if result.stderr:
                    print(result.stderr.rstrip(), file=sys.stderr)
        except (OSError, subprocess.SubprocessError) as error:
            failures.append(f"{relative}: {type(error).__name__}: {error}")
            print(f"[{label}] FAIL: {relative}: {type(error).__name__}: {error}")

    return BatchResult(passed, len(selected), tuple(failures))


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _log_sections(text: str) -> dict[str, str]:
    markers = list(re.finditer(r"^===== (.+?) =====\s*$", text, re.MULTILINE))
    sections: dict[str, str] = {}
    for index, marker in enumerate(markers):
        end = markers[index + 1].start() if index + 1 < len(markers) else len(text)
        sections[marker.group(1).strip()] = text[marker.end():end]
    return sections


def _return_code(sections: dict[str, str], label: str) -> int | None:
    content = sections.get(label)
    if content is None:
        return None
    matches = re.findall(r"^RETURN CODE:\s*(-?\d+)\s*$", content, re.MULTILINE)
    return int(matches[-1]) if matches else None


def _summary_value(summary: str, label: str) -> str | None:
    match = re.search(rf"^{re.escape(label)}:\s*(.*?)\s*$", summary, re.MULTILINE | re.IGNORECASE)
    return match.group(1) if match else None


def _software_stages(
    summary: str,
    software_log: str,
) -> tuple[StageResult, StageResult, StageResult]:
    sections = _log_sections(software_log)
    software_state = (_summary_value(summary, "Software tests") or "NOT RUN").upper()

    host_labels = [f"{action} {name}" for name in C_HOST_TESTS for action in ("compile", "run")]
    host_codes = [_return_code(sections, label) for label in host_labels]
    completed_host = sum(code is not None for code in host_codes)
    attempted_host = sum(label in sections for label in host_labels)
    if (any(code not in (None, 0) for code in host_codes)
            or any(label in sections and code is None for label, code in zip(host_labels, host_codes))):
        host = StageResult("FAIL", f"{sum(code == 0 for code in host_codes)}/10 compile/run steps passed")
    elif completed_host == len(host_codes):
        host = StageResult("PASS", "5/5 C host tests")
    elif attempted_host == 0 and software_state in ("NOT RUN", "SKIPPED"):
        host = _not_run("Level 3 software phase did not run") if software_state == "NOT RUN" else StageResult("SKIPPED")
    else:
        host = StageResult("FAIL", f"incomplete C host evidence ({completed_host}/10 steps)")

    python_code = _return_code(sections, "Python unittest discovery")
    if python_code == 0:
        python_tests = StageResult("PASS")
    elif python_code is not None:
        python_tests = StageResult("FAIL", f"exit {python_code}")
    elif "Python unittest discovery" in sections:
        python_tests = StageResult("FAIL", "Python unittest command has no return-code evidence")
    elif software_state == "PASS":
        python_tests = StageResult("FAIL", "missing Python unittest log section")
    elif software_state == "SKIPPED":
        python_tests = StageResult("SKIPPED")
    else:
        python_tests = _not_run("Level 3 software phase did not reach Python tests")

    wiring_codes = [_return_code(sections, f"wiring {name}") for name in WIRING_TESTS]
    completed_wiring = sum(code is not None for code in wiring_codes)
    wiring_labels = [f"wiring {name}" for name in WIRING_TESTS]
    attempted_wiring = sum(label in sections for label in wiring_labels)
    if (any(code not in (None, 0) for code in wiring_codes)
            or any(label in sections and code is None for label, code in zip(wiring_labels, wiring_codes))):
        wiring = StageResult("FAIL", f"{sum(code == 0 for code in wiring_codes)}/3 wiring tests passed")
    elif completed_wiring == len(wiring_codes):
        wiring = StageResult("PASS", "3/3 tests")
    elif attempted_wiring == 0 and software_state in ("NOT RUN", "SKIPPED"):
        wiring = _not_run("Level 3 software phase did not run") if software_state == "NOT RUN" else StageResult("SKIPPED")
    else:
        wiring = StageResult("FAIL", f"incomplete wiring evidence ({completed_wiring}/3)")
    return host, python_tests, wiring


def _parse_rebuild(summary: str, build_log: str, software_stages: Iterable[StageResult]) -> StageResult:
    marker = _summary_value(summary, "Build")
    if marker and marker.upper().startswith("SKIPPED"):
        return StageResult("SKIPPED", marker)
    if marker and marker.upper().startswith("PASS"):
        sections = _log_sections(build_log)
        rc = _return_code(sections, "Keil process")
        footers = list(BUILD_FOOTER.finditer(build_log))
        if rc == 0 and footers and footers[-1].group("errors") == "0" and footers[-1].group("warnings") == "0":
            return StageResult("PASS", "0 errors, 0 warnings; Level 3 log confirmed")
        return StageResult("FAIL", "summary/log evidence for a clean Rebuild is incomplete")
    if all(stage.status == "PASS" for stage in software_stages):
        return StageResult("FAIL", "Level 3 software passed but Rebuild did not report PASS")
    return _not_run("Level 3 software phase did not pass; Rebuild was gated")


def _parse_flash(summary: str, flash_log: str, rebuild: StageResult) -> StageResult:
    marker = _summary_value(summary, "Flash")
    if marker and marker.upper().startswith("SKIPPED"):
        return StageResult("SKIPPED", marker)
    if marker and marker.upper().startswith("PASS"):
        sections = _log_sections(flash_log)
        rc_values = re.findall(r"^RETURN CODE:\s*(-?\d+)\s*$", flash_log, re.MULTILINE)
        output = "\n".join(sections.values()) + "\n" + flash_log
        verified = bool(re.search(r"download\s+verified\s+successfully|verification[^\r\n]*success|verify[^\r\n]*success", output, re.I))
        reset = bool(re.search(r"\bMCU\s+Reset\b|\breset[^\r\n]*(?:success|complete|done)", output, re.I))
        run = bool(re.search(r"\bCore\s+run\b|\brun[^\r\n]*(?:success|complete|done|running)", output, re.I))
        if rc_values and int(rc_values[-1]) == 0 and verified and reset and run:
            return StageResult("PASS", "Verify, Reset, Run confirmed in Level 3 log")
        return StageResult("FAIL", "Flash/Verify/Reset/Run evidence is incomplete")
    if rebuild.status == "PASS":
        return StageResult("FAIL", "Rebuild passed but Flash did not report PASS")
    return _not_run("Level 3 did not reach Flash")


def _case_records(texts: Iterable[str]) -> dict[str, str]:
    records: dict[str, str] = {}
    for text in texts:
        for match in re.finditer(
            r"^CASE\s+(\S+)\s+(PASS|FAIL)(?:\s*:.*)?\s*$", text, re.MULTILINE | re.IGNORECASE
        ):
            name, status = Path(match.group(1)).name, match.group(2).upper()
            records[name] = "FAIL" if status == "FAIL" or records.get(name) == "FAIL" else "PASS"
    return records


def _parse_hardware_cases(summary: str, hardware_log: str, flash: StageResult) -> StageResult:
    expected_value = _summary_value(summary, "Cases")
    expected = tuple(Path(item.strip()).name for item in (expected_value or "").split(",") if item.strip())
    records = _case_records((hardware_log, summary))
    phase_pass = bool(re.search(r"^Hardware cases:\s*PASS\b", summary, re.MULTILINE | re.IGNORECASE))
    if expected and any(records.get(name) == "FAIL" for name in expected):
        count = sum(records.get(name) == "PASS" for name in expected)
        return StageResult("FAIL", f"{count}/{len(expected)} cases passed")
    if expected and phase_pass and all(records.get(name) == "PASS" for name in expected):
        return StageResult("PASS", f"{len(expected)}/{len(expected)} cases")
    attempted = bool(records) or "===== Runner " in hardware_log
    if attempted or (phase_pass and not expected):
        count = sum(records.get(name) == "PASS" for name in expected)
        total = len(expected)
        detail = f"incomplete case evidence ({count}/{total})" if total else "expected case list is missing"
        return StageResult("FAIL", detail)
    if flash.status == "PASS":
        return StageResult("FAIL", "Flash passed but hardware cases did not report complete results")
    return _not_run("Level 3 did not reach STM32 hardware cases")


def _parse_stack_guard(summary: str, hardware_log: str, cases: StageResult) -> StageResult:
    lines = (summary + "\n" + hardware_log).splitlines()
    guards: list[int] = []
    for line in lines:
        if "[STACK]" in line.upper() or re.search(r"\bSTACK\s*:", line, re.I):
            match = re.search(r"\bGUARD\s*=\s*(\d+)\b", line, re.I)
            if match:
                guards.append(int(match.group(1)))
    if any(value == 0 for value in guards):
        return StageResult("FAIL", "GUARD=0 reported by Level 3")
    if guards:
        return StageResult("PASS", f"{len(guards)} watermark(s), GUARD nonzero")
    if cases.status != "NOT RUN":
        return StageResult("INFO", "watermark unavailable; follows Level 3 informational policy")
    return _not_run("hardware cases did not run")


def parse_level3_artifacts(run_dir: Path | None, process_returncode: int) -> Level3Result:
    if run_dir is None:
        missing = _not_run("Level 3 artifacts were not created")
        return Level3Result(missing, missing, missing, missing, missing, missing, missing,
                            StageResult("FAIL", f"Level 3 process exit {process_returncode}; no summary evidence"))

    summary = _read_text(run_dir / "summary.txt")
    software_log = _read_text(run_dir / "software_tests.log")
    build_log = _read_text(run_dir / "build.log")
    flash_log = _read_text(run_dir / "flash.log")
    hardware_log = _read_text(run_dir / "hardware.log")
    firmware_host, python_tests, wiring = _software_stages(summary, software_log)
    rebuild = _parse_rebuild(summary, build_log, (firmware_host, python_tests, wiring))
    flash = _parse_flash(summary, flash_log, rebuild)
    cases = _parse_hardware_cases(summary, hardware_log, flash)
    stack_guard = _parse_stack_guard(summary, hardware_log, cases)

    explicit_l3_pass = bool(re.search(r"^LEVEL 3 PASS\s*$", summary, re.MULTILINE))
    explicit_status_pass = bool(re.search(r"^Status:\s*PASS\s*$", summary, re.MULTILINE | re.IGNORECASE))
    hard_stages_pass = all(stage.status == "PASS" for stage in (
        firmware_host, python_tests, wiring, rebuild, flash, cases,
    )) and stack_guard.status in ("PASS", "INFO")
    if process_returncode == 0 and explicit_l3_pass and explicit_status_pass and hard_stages_pass:
        overall = StageResult("PASS", "LEVEL 3 PASS and stage evidence agree")
    else:
        overall = StageResult("FAIL", f"Level 3 exit {process_returncode}; summary/log evidence did not prove full PASS")
    return Level3Result(firmware_host, python_tests, wiring, rebuild, flash, cases, stack_guard,
                        overall, run_dir)


def run_level3(port: str, repo_root: Path = ROOT) -> Level3Result:
    artifact_root = repo_root / "artifacts" / "hardware_test"
    before = {path.resolve() for path in artifact_root.iterdir() if path.is_dir()} if artifact_root.is_dir() else set()
    argv = [sys.executable, str(repo_root / "tools" / "glove_test" / "test_hardware.py"), "--port", port]
    try:
        process = subprocess.Popen(argv, cwd=str(repo_root))
        returncode = process.wait()
    except OSError as error:
        print(f"[PROJECT] Could not start Level 3: {error}", file=sys.stderr)
        return parse_level3_artifacts(None, 1)

    after = [path.resolve() for path in artifact_root.iterdir() if path.is_dir()] if artifact_root.is_dir() else []
    created = [path for path in after if path not in before]
    pid_matches = [path for path in created if re.search(rf"_{process.pid}(?:_\d+)?$", path.name)]
    run_dir = pid_matches[0] if len(pid_matches) == 1 else created[0] if len(created) == 1 else None
    if run_dir is None:
        print("[PROJECT] Level 3 run directory could not be uniquely identified", file=sys.stderr)
    return parse_level3_artifacts(run_dir, returncode)


def level3_after_software(
    mini_program: BatchResult,
    focused: BatchResult,
    port: str,
    runner: Callable[[str], Level3Result],
) -> Level3Result | None:
    if not mini_program.passed or not focused.passed:
        return None
    return runner(port)


def _stage_line(label: str, stage: StageResult) -> str:
    suffix = f" ({stage.detail})" if stage.detail else ""
    return f"{label}: {stage.status}{suffix}"


def _project_passed(mini_program: BatchResult, focused: BatchResult, level3: Level3Result | None) -> bool:
    if not mini_program.passed or not focused.passed or level3 is None:
        return False
    required = (
        level3.firmware_host, level3.python_tests, level3.wiring,
        level3.rebuild, level3.flash, level3.hardware_cases,
    )
    return (
        all(stage.status == "PASS" for stage in required)
        and level3.stack_guard.status in ("PASS", "INFO")
        and level3.overall.status == "PASS"
    )


def render_summary(mini_program: BatchResult, focused: BatchResult, level3: Level3Result | None) -> str:
    mini_status = "PASS" if mini_program.passed else "FAIL"
    focused_status = "PASS" if focused.passed else "FAIL"
    lines = [
        f"MiniProgram Tests: {mini_program.passed_files}/{mini_program.total_files} files {mini_status}",
        f"Focused Regression: {focused.passed_files}/{focused.total_files} files {focused_status}",
    ]
    if level3 is None:
        detail = "software gate blocked Level 3"
        not_run = _not_run(detail)
        stages = Level3Result(not_run, not_run, not_run, not_run, not_run, not_run, not_run,
                              StageResult("FAIL", detail))
    else:
        stages = level3

    lines.extend((
        _stage_line("Firmware Host Tests", stages.firmware_host),
        _stage_line("Python Unit Tests", stages.python_tests),
        _stage_line("Static/Wiring Tests", stages.wiring),
        _stage_line("Keil Rebuild", stages.rebuild),
        _stage_line("ST-Link Flash", stages.flash),
        _stage_line("STM32 Hardware Cases", stages.hardware_cases),
        _stage_line("Stack Guard", stages.stack_guard),
    ))
    overall = "PASS" if _project_passed(mini_program, focused, level3) else "FAIL"
    lines.append(f"PROJECT REGRESSION {overall}")
    if stages.artifact_dir is not None:
        lines.append(f"Level 3 artifacts: {stages.artifact_dir}")
    return "\n".join(lines)


def _port_value(value: str) -> str:
    if not re.fullmatch(r"COM\d+", value.strip(), re.IGNORECASE):
        raise argparse.ArgumentTypeError("port must look like COM15")
    return value.strip().upper()


def run_project_regression(port: str = "COM15", repo_root: Path = ROOT) -> int:
    full_files = enumerate_miniprogram_tests(repo_root / "miniprogram" / "test")
    mini_result = run_node_file_suite(full_files, repo_root, label="MiniProgram")

    focus_files, missing_focus = focused_test_files(repo_root / "miniprogram" / "test")
    focused_result = run_node_file_suite(focus_files, repo_root, label="Focused")
    if missing_focus:
        focused_result = BatchResult(
            focused_result.passed_files,
            len(FOCUSED_TEST_FILES),
            focused_result.failures + tuple(f"required focused test is missing: {name}" for name in missing_focus),
        )
    level3 = level3_after_software(mini_result, focused_result, port,
                                   lambda selected_port: run_level3(selected_port, repo_root))
    summary = render_summary(mini_result, focused_result, level3)
    print("\n" + summary)
    return 0 if _project_passed(mini_result, focused_result, level3) else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=_port_value, default="COM15",
                        help="USB-TTL serial port for Level 3 hardware cases (default: COM15)")
    args = parser.parse_args(argv)
    return run_project_regression(args.port)


if __name__ == "__main__":
    raise SystemExit(main())
