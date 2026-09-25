"""Mock-only checks for the project regression summary and software gate."""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "test_all.py"
SPEC = importlib.util.spec_from_file_location("project_regression", MODULE_PATH)
REGRESSION = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = REGRESSION
SPEC.loader.exec_module(REGRESSION)


def successful_logs(root: Path) -> None:
    root.mkdir(parents=True)
    (root / "summary.txt").write_text(
        """Software tests: PASS
Cases: alpha.json, beta.json
Build: PASS; 0 Error(s), 0 Warning(s); HEX SHA-256=abc
Flash: PASS (verify/reset/run confirmed)
STACK: SIZE=4096 USED=1000 FREE=3096 GUARD=1
Hardware cases: PASS
Status: PASS
CASE alpha.json PASS
CASE beta.json PASS
LEVEL 3 PASS
""",
        encoding="utf-8",
    )
    software = []
    for name in REGRESSION.C_HOST_TESTS:
        for action in ("compile", "run"):
            software.append(f"===== {action} {name} =====\nRETURN CODE: 0\n")
    software.append("===== Python unittest discovery =====\nRETURN CODE: 0\n")
    for name in REGRESSION.WIRING_TESTS:
        software.append(f"===== wiring {name} =====\nRETURN CODE: 0\n")
    (root / "software_tests.log").write_text("\n".join(software), encoding="utf-8")
    (root / "build.log").write_text(
        "0 Error(s), 0 Warning(s)\n===== Keil process =====\nRETURN CODE: 0\n",
        encoding="utf-8",
    )
    (root / "flash.log").write_text(
        "===== STM32 Programmer flash =====\nRETURN CODE: 0\n"
        "Download verified successfully\nMCU Reset\nCore run\n",
        encoding="utf-8",
    )
    (root / "hardware.log").write_text(
        "CASE alpha.json PASS\nCASE beta.json PASS\n"
        "[STACK]|SIZE=4096|USED=1000|FREE=3096|GUARD=1\n",
        encoding="utf-8",
    )


class ProjectRegressionTests(unittest.TestCase):
    def test_artifact_summary_requires_evidence_for_each_level3_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            successful_logs(run_dir)
            result = REGRESSION.parse_level3_artifacts(run_dir, 0)

        self.assertEqual(result.firmware_host.status, "PASS")
        self.assertEqual(result.python_tests.status, "PASS")
        self.assertEqual(result.wiring.status, "PASS")
        self.assertEqual(result.rebuild.status, "PASS")
        self.assertEqual(result.flash.status, "PASS")
        self.assertEqual(result.hardware_cases.status, "PASS")
        self.assertEqual(result.stack_guard.status, "PASS")
        self.assertEqual(result.overall.status, "PASS")
        summary = REGRESSION.render_summary(
            REGRESSION.BatchResult(30, 30),
            REGRESSION.BatchResult(17, 17),
            result,
        )
        for line in (
            "MiniProgram Tests: 30/30 files PASS",
            "Focused Regression: 17/17 files PASS",
            "Firmware Host Tests: PASS",
            "Python Unit Tests: PASS",
            "Static/Wiring Tests: PASS",
            "Keil Rebuild: PASS",
            "ST-Link Flash: PASS",
            "STM32 Hardware Cases: PASS (2/2 cases)",
            "Stack Guard: PASS",
            "PROJECT REGRESSION PASS",
        ):
            self.assertIn(line, summary)

    def test_guard_zero_fails_and_missing_watermark_is_informational(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            successful_logs(run_dir)
            summary = run_dir / "summary.txt"
            summary_text = summary.read_text(encoding="utf-8")
            summary_text = summary_text.replace(
                "STACK: SIZE=4096 USED=1000 FREE=3096 GUARD=1",
                "STACK: SIZE=4096 USED=1000 FREE=3096 GUARD=0",
            )
            summary_text = summary_text.replace("Status: PASS", "Status: FAIL").replace("LEVEL 3 PASS", "LEVEL 3 FAIL")
            summary.write_text(summary_text, encoding="utf-8")
            (run_dir / "hardware.log").write_text(
                "CASE alpha.json FAIL: [STACK] GUARD=0\nCASE beta.json PASS\n",
                encoding="utf-8",
            )
            failed = REGRESSION.parse_level3_artifacts(run_dir, 1)
            summary_text = summary_text.replace(
                "STACK: SIZE=4096 USED=1000 FREE=3096 GUARD=0",
                "STACK: watermark unavailable (informational; no minimum FREE threshold)",
            ).replace("CASE alpha.json FAIL: [STACK] GUARD=0", "CASE alpha.json PASS")
            summary_text = summary_text.replace("Status: FAIL", "Status: PASS").replace("LEVEL 3 FAIL", "LEVEL 3 PASS")
            summary.write_text(summary_text, encoding="utf-8")
            (run_dir / "hardware.log").write_text(
                "CASE alpha.json PASS\nCASE beta.json PASS\n", encoding="utf-8"
            )
            informational = REGRESSION.parse_level3_artifacts(run_dir, 0)

        self.assertEqual(failed.hardware_cases.status, "FAIL")
        self.assertEqual(failed.stack_guard.status, "FAIL")
        self.assertEqual(failed.overall.status, "FAIL")
        self.assertEqual(informational.stack_guard.status, "INFO")

    def test_level3_software_failure_keeps_build_and_flash_not_run(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            successful_logs(run_dir)
            summary = """Smart Glove Level 3 hardware test
Cases: alpha.json, beta.json
Preflight: PASS
Failure: HardwareTestError: run alarm_engine_test failed with return code 1
Status: FAIL
LEVEL 3 FAIL
"""
            (run_dir / "summary.txt").write_text(summary, encoding="utf-8")
            software = (run_dir / "software_tests.log").read_text(encoding="utf-8")
            software = software.replace("===== run alarm_engine_test =====\nRETURN CODE: 0",
                                        "===== run alarm_engine_test =====\nRETURN CODE: 1")
            software = software.split("===== compile bluetooth_tx_test =====", maxsplit=1)[0]
            (run_dir / "software_tests.log").write_text(software, encoding="utf-8")
            (run_dir / "hardware.log").write_text("Level 3 hardware log\n", encoding="utf-8")
            result = REGRESSION.parse_level3_artifacts(run_dir, 1)

        self.assertEqual(result.firmware_host.status, "FAIL")
        self.assertEqual(result.python_tests.status, "NOT RUN")
        self.assertEqual(result.wiring.status, "NOT RUN")
        self.assertEqual(result.rebuild.status, "NOT RUN")
        self.assertEqual(result.flash.status, "NOT RUN")
        self.assertEqual(result.hardware_cases.status, "NOT RUN")
        self.assertEqual(result.overall.status, "FAIL")

    def test_software_failure_gates_level3_and_summary_keeps_unrun_stages_unrun(self):
        mini = REGRESSION.BatchResult(29, 30, ("failed.test.js",))
        focused = REGRESSION.BatchResult(17, 17)
        level3_runner = Mock(side_effect=AssertionError("Level 3 must not run"))
        result = REGRESSION.level3_after_software(mini, focused, "COM15", level3_runner)
        summary = REGRESSION.render_summary(mini, focused, result)

        level3_runner.assert_not_called()
        self.assertIn("MiniProgram Tests: 29/30 files FAIL", summary)
        self.assertIn("Focused Regression: 17/17 files PASS", summary)
        self.assertIn("Firmware Host Tests: NOT RUN", summary)
        self.assertIn("Keil Rebuild: NOT RUN", summary)
        self.assertIn("ST-Link Flash: NOT RUN", summary)
        self.assertIn("PROJECT REGRESSION FAIL", summary)

    def test_level3_is_called_only_when_both_node_batches_pass(self):
        mini = REGRESSION.BatchResult(30, 30)
        focused = REGRESSION.BatchResult(16, 17, ("missing file",))
        level3_runner = Mock()
        self.assertIsNone(REGRESSION.level3_after_software(mini, focused, "COM15", level3_runner))
        level3_runner.assert_not_called()

        focused = REGRESSION.BatchResult(17, 17)
        expected = object()
        level3_runner.return_value = expected
        self.assertIs(REGRESSION.level3_after_software(mini, focused, "COM15", level3_runner), expected)
        level3_runner.assert_called_once_with("COM15")


if __name__ == "__main__":
    unittest.main(verbosity=2)
