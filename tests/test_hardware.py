import importlib.util
import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "glove_test" / "test_hardware.py"
SPEC = importlib.util.spec_from_file_location("glove_hardware", MODULE_PATH)
HARDWARE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = HARDWARE
SPEC.loader.exec_module(HARDWARE)


def completed(returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class FakePort:
    def __init__(self, device, description="USB-SERIAL CH340"):
        self.device = device
        self.description = description
        self.manufacturer = ""
        self.product = ""
        self.hwid = ""


class HardwareUnitTests(unittest.TestCase):
    def make_project(self, root, device="STM32F407ZGTx", output_dir="shuangshou\\"):
        mdk = root / "firmware" / "stm32f407" / "MDK-ARM"
        mdk.mkdir(parents=True)
        project = mdk / "shuangshou.uvprojx"
        project.write_text(
            """<?xml version=\"1.0\"?>
<Project><Targets><Target><TargetName>shuangshou</TargetName>
<TargetOption><TargetCommonOption>
<Device>{device}</Device><OutputDirectory>{output_dir}</OutputDirectory>
<OutputName>shuangshou</OutputName><CreateHexFile>1</CreateHexFile>
</TargetCommonOption></TargetOption></Target></Targets></Project>""".format(
                device=device, output_dir=output_dir
            ),
            encoding="utf-8",
        )
        return HARDWARE.load_project_info(project, root / "firmware" / "stm32f407")

    def make_args(self, **overrides):
        values = dict(
            port="COM15",
            baud=9600,
            case=None,
            all_cases=False,
            keil=None,
            programmer=None,
            probe_serial=None,
            skip_software_tests=True,
            skip_build=True,
            skip_flash=True,
        )
        values.update(overrides)
        return types.SimpleNamespace(**values)

    def test_select_probe_deduplicates_vcp_and_stlink_sections(self):
        output = """
STLink probes:
  Serial Number : 00430052310000024E593053
STLink VCP (COM8):
  Serial Number : 00430052310000024E593053
"""
        serials = HARDWARE.parse_stlink_serials(output)
        self.assertEqual(serials, ["00430052310000024E593053"])
        self.assertEqual(
            HARDWARE.select_probe_serial(serials),
            "00430052310000024E593053",
        )

    def test_multiple_and_missing_probe_selection_fail(self):
        with self.assertRaises(HARDWARE.HardwareTestError):
            HARDWARE.select_probe_serial(["AAAABBBB", "CCCCDDDD"])
        with self.assertRaises(HARDWARE.HardwareTestError):
            HARDWARE.select_probe_serial(["AAAABBBB"], "MISSING")
        self.assertEqual(
            HARDWARE.select_probe_serial(["AAAABBBB", "CCCCDDDD"], "ccccdddd"),
            "CCCCDDDD",
        )

    def test_project_hex_is_read_from_xml_and_must_stay_in_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            self.assertEqual(
                info.hex_path,
                (root / "firmware/stm32f407/MDK-ARM/shuangshou/shuangshou.hex").resolve(),
            )
            with self.assertRaises(HARDWARE.HardwareTestError):
                self.make_project(root / "other", output_dir="../../../outside")

    def test_wrong_device_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(HARDWARE.HardwareTestError):
                self.make_project(Path(directory), device="STM32F103C8Tx")

    def test_skip_build_requires_skip_flash(self):
        with self.assertRaises(SystemExit):
            HARDWARE.main(["--port", "COM15", "--skip-build"])
        with mock.patch.object(HARDWARE, "run_workflow") as workflow:
            workflow.return_value = (0, Path("run"))
            self.assertEqual(
                HARDWARE.main(["--port", "COM15", "--skip-build", "--skip-flash"]),
                0,
            )
            workflow.assert_called_once()

    def test_preflight_port_rejects_stlink_vcp(self):
        with self.assertRaises(HARDWARE.HardwareTestError):
            HARDWARE.validate_serial_port(
                "COM8", [FakePort("COM8", "ST-Link VCP")]
            )
        HARDWARE.validate_serial_port("COM15", [FakePort("COM15")])

    def test_stale_hex_and_build_warnings_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            info.hex_path.parent.mkdir(parents=True)
            info.hex_path.write_text(":00000001FF\n", encoding="ascii")
            before = info.hex_path.stat().st_mtime_ns
            build_log = root / "build.log"
            build_log.write_text(
                "Build complete: 0 Error(s), 1 Warning(s)\n", encoding="utf-8"
            )
            fake = completed(0, "", "")
            with mock.patch.object(HARDWARE, "_invoke", return_value=fake), mock.patch.object(
                HARDWARE, "_wait_for_build_footer", return_value=build_log.read_text()
            ):
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "zero-error"):
                    HARDWARE.build_firmware(info, Path("UV4.exe"), build_log)

            build_log.write_text(
                "Build complete: 0 Error(s), 0 Warning(s)\n", encoding="utf-8"
            )
            with mock.patch.object(HARDWARE, "_invoke", return_value=fake), mock.patch.object(
                HARDWARE, "_wait_for_build_footer", return_value=build_log.read_text()
            ), mock.patch.object(HARDWARE, "time") as fake_time:
                fake_time.time_ns.return_value = info.hex_path.stat().st_mtime_ns + 10**9
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "stale"):
                    HARDWARE.build_firmware(info, Path("UV4.exe"), build_log)

            def changed_but_old(command, cwd, timeout=None):
                info.hex_path.write_text(":changed\n", encoding="ascii")
                os.utime(info.hex_path, ns=(before - 1000, before - 1000))
                return fake

            build_log.write_text("0 Error(s), 0 Warning(s)\n", encoding="utf-8")
            with mock.patch.object(HARDWARE, "_invoke", side_effect=changed_but_old), mock.patch.object(
                HARDWARE, "_wait_for_build_footer", return_value=build_log.read_text()
            ), mock.patch.object(HARDWARE, "time") as fake_time:
                fake_time.time_ns.return_value = before
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "not newer"):
                    HARDWARE.build_firmware(info, Path("UV4.exe"), build_log)

    def test_build_success_requires_fresh_hex_and_records_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            info.hex_path.parent.mkdir(parents=True)
            info.hex_path.write_text(":00000001FF\n", encoding="ascii")
            before = info.hex_path.stat().st_mtime_ns
            build_log = root / "build.log"
            build_log.write_text("0 Error(s), 0 Warning(s)\n", encoding="utf-8")

            def fake_invoke(command, cwd, timeout=None):
                info.hex_path.write_text(":00000001FF\n:00000000FF\n", encoding="ascii")
                fresh = HARDWARE.time.time_ns() + 10**6
                os.utime(info.hex_path, ns=(fresh, fresh))
                return completed(0, "Keil done", "")

            with mock.patch.object(HARDWARE, "_invoke", side_effect=fake_invoke), mock.patch.object(
                HARDWARE, "_wait_for_build_footer", return_value=build_log.read_text()
            ):
                result = HARDWARE.build_firmware(info, Path("UV4.exe"), build_log)
            self.assertEqual(result, HARDWARE.sha256_file(info.hex_path))

    def test_flash_command_and_missing_verify_are_safe_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            info.hex_path.parent.mkdir(parents=True)
            info.hex_path.write_text(":00000001FF\n", encoding="ascii")
            flash_log = root / "flash.log"
            result = completed(0, "Programming successful\n", "")
            with mock.patch.object(HARDWARE, "_invoke", return_value=result) as invoke:
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "confirm"):
                    HARDWARE.flash_firmware(
                        info, Path("STM32_Programmer_CLI.exe"), "SERIAL", flash_log
                    )
            command = invoke.call_args.args[0]
            self.assertIn("-d", command)
            self.assertIn("-rst", command)
            self.assertIn("-run", command)
            self.assertNotIn("-e", command)

    def test_flash_success_requires_verify_reset_and_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            info.hex_path.parent.mkdir(parents=True)
            info.hex_path.write_text(":00000001FF\n", encoding="ascii")
            flash_log = root / "flash.log"
            result = completed(
                0,
                "Download verified successfully\nMCU Reset\nCore run\n",
                "",
            )
            with mock.patch.object(HARDWARE, "_invoke", return_value=result):
                HARDWARE.flash_firmware(
                    info, Path("STM32_Programmer_CLI.exe"), "SERIAL", flash_log
                )

    def test_runner_case_requires_zero_exit_pass_and_exit_confirmation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "run_virtual_sensor_test.py"
            runner.write_text("", encoding="utf-8")
            case = root / "mixed.json"
            case.write_text("{}", encoding="utf-8")
            log = root / "hardware.log"
            outputs = [
                completed(0, "PASS: mixed\n", ""),
            ]
            with mock.patch.object(HARDWARE, "_invoke", side_effect=outputs):
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "EXIT confirmation"):
                    HARDWARE.run_hardware_cases(root, runner, [case], "COM15", 9600, log)

    def test_runner_case_failure_and_guard_zero_fail_even_with_zero_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "run_virtual_sensor_test.py"
            case = root / "mixed.json"
            runner.write_text("", encoding="utf-8")
            case.write_text("{}", encoding="utf-8")
            log = root / "hardware.log"
            result = completed(
                0,
                "[TEST] MODE=REAL\n[STACK]|SIZE=4096|USED=984|FREE=3112|GUARD=0\nPASS: mixed\n",
                "",
            )
            with mock.patch.object(HARDWARE, "_invoke", return_value=result):
                with self.assertRaisesRegex(HARDWARE.HardwareTestError, "GUARD=0"):
                    HARDWARE.run_hardware_cases(root, runner, [case], "COM15", 9600, log)

    def test_software_failure_stops_before_build_and_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self.make_project(root)
            log = root / "software.log"
            tools = HARDWARE.ToolPaths(Path("UV4.exe"), Path("CLI.exe"), Path("gcc.exe"), Path("powershell.exe"))
            result = completed(1, "regression failed", "")
            with mock.patch.object(HARDWARE, "_invoke", return_value=result) as invoke:
                with self.assertRaises(HARDWARE.HardwareTestError):
                    HARDWARE.run_software_tests(root, info, tools, log)
            self.assertEqual(invoke.call_count, 1)

    def test_runner_invoked_once_per_case_and_watermarks_are_informational(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "run_virtual_sensor_test.py"
            runner.write_text("", encoding="utf-8")
            cases = []
            for name in ("a.json", "b.json"):
                case = root / name
                case.write_text("{}", encoding="utf-8")
                cases.append(case)
            log = root / "hardware.log"
            result = completed(
                0,
                "[TEST] MODE=REAL\nPASS: case\n"
                "[STACK]|SIZE=4096|USED=984|FREE=3112|GUARD=1\n",
                "",
            )
            with mock.patch.object(HARDWARE, "_invoke", return_value=result) as invoke:
                watermarks = HARDWARE.run_hardware_cases(
                    root, runner, cases, "COM15", 9600, log
                )
            self.assertEqual(invoke.call_count, 2)
            self.assertEqual(watermarks[0].free, 3112)

    def test_exit_codes_are_nonzero_on_workflow_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(HARDWARE, "_repo_root", return_value=root), mock.patch.object(
                HARDWARE, "preflight", side_effect=HARDWARE.HardwareTestError("bad preflight")
            ):
                status = HARDWARE.main(["--port", "COM15", "--skip-build", "--skip-flash"])
            self.assertNotEqual(status, 0)

    def test_confirmed_exit_case_failure_continues_to_next_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = root / "run_virtual_sensor_test.py"
            runner.write_text("", encoding="utf-8")
            cases = [root / "a.json", root / "b.json"]
            for case in cases:
                case.write_text("{}", encoding="utf-8")
            output = [
                completed(1, "[TEST] MODE=REAL\nFAIL: a\n", ""),
                completed(0, "[TEST] MODE=REAL\nPASS: b\n", ""),
            ]
            with mock.patch.object(HARDWARE, "_invoke", side_effect=output) as invoke:
                with self.assertRaises(HARDWARE.HardwareTestError):
                    HARDWARE.run_hardware_cases(root, runner, cases, "COM15", 9600, root / "hardware.log")
            self.assertEqual(invoke.call_count, 2)

    def _workflow_info(self, root):
        project = self.make_project(root)
        runner = root / "run_virtual_sensor_test.py"
        case = root / "mixed.json"
        runner.write_text("", encoding="utf-8")
        case.write_text("{}", encoding="utf-8")
        tools = HARDWARE.ToolPaths(Path("UV4.exe"), Path("CLI.exe"), Path("gcc.exe"), Path("powershell.exe"))
        return HARDWARE.PreflightInfo(project, runner, (case,), tools, "SERIAL", "listing")

    def test_workflow_never_flashes_after_software_or_build_failure(self):
        for failing_stage in ("software", "build"):
            with self.subTest(stage=failing_stage), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                info = self._workflow_info(root)
                args = self.make_args(skip_software_tests=False, skip_build=False, skip_flash=False)
                failure = HARDWARE.HardwareTestError(failing_stage)
                with mock.patch.object(HARDWARE, "preflight", return_value=info), mock.patch.object(
                    HARDWARE, "run_software_tests"
                ) as software, mock.patch.object(HARDWARE, "build_firmware") as build, mock.patch.object(
                    HARDWARE, "flash_firmware"
                ) as flash:
                    if failing_stage == "software":
                        software.side_effect = failure
                    else:
                        software.return_value = None
                        build.side_effect = failure
                    status, _ = HARDWARE.run_workflow(args, root)
                self.assertNotEqual(status, 0)
                flash.assert_not_called()

    def test_workflow_success_reports_build_counts_and_overall_result(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = self._workflow_info(root)
            args = self.make_args(skip_software_tests=False, skip_build=False, skip_flash=False)
            with mock.patch.object(HARDWARE, "preflight", return_value=info), mock.patch.object(
                HARDWARE, "run_software_tests"), mock.patch.object(
                HARDWARE, "build_firmware", return_value="abc123"), mock.patch.object(
                HARDWARE, "flash_firmware"), mock.patch.object(
                HARDWARE, "run_hardware_cases", return_value=[HARDWARE.StackWatermark(4096, 984, 3112, 1)]
            ), mock.patch.object(HARDWARE.time, "sleep"), mock.patch.object(
                HARDWARE, "_git_sha", return_value="deadbeef"
            ):
                status, run_dir = HARDWARE.run_workflow(args, root)
            summary = (run_dir / "summary.txt").read_text(encoding="utf-8")
            self.assertEqual(status, 0)
            self.assertIn("0 Error(s), 0 Warning(s)", summary)
            self.assertIn("Git SHA: deadbeef", summary)
            self.assertIn("LEVEL 3 PASS", summary)


if __name__ == "__main__":
    unittest.main(verbosity=2)
