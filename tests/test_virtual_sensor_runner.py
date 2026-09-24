import importlib.util
import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "tools" / "glove_test" / "run_virtual_sensor_test.py"
SPEC = importlib.util.spec_from_file_location("glove_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class FakeSerial:
    def __init__(self, case, confirm_exit=True, miss_responses=None,
                 telemetry_after_apply=None):
        self.case = case
        self.confirm_exit = confirm_exit
        self.miss_responses = dict(miss_responses or {})
        self.telemetry_after_apply = telemetry_after_apply
        self.lines = []
        self.writes = []

    def reset_input_buffer(self):
        self.lines.clear()

    def flush(self):
        pass

    def write(self, payload):
        command = payload.decode("ascii").strip()
        self.writes.append(command)
        replies = {
            "TEST:ENTER": "[TEST] MODE=VIRTUAL",
            "TEST:FLEX": "[TEST] FLEX=OK",
            "TEST:IMU": "[TEST] IMU=OK",
            "TEST:ACC": "[TEST] ACC=OK",
            "TEST:APPLY": "[TEST] APPLY=OK",
            "TEST:EXIT": "[TEST] MODE=REAL",
        }
        key = next(item for item in replies if command.startswith(item))
        if self.miss_responses.get(key, 0) > 0:
            self.miss_responses[key] -= 1
            return len(payload)
        if key != "TEST:EXIT" or self.confirm_exit:
            self.lines.append((replies[key] + "\r\n").encode("ascii"))
        if key == "TEST:APPLY":
            flex = self.case["flex"]
            imu = self.case["imu"]
            acc = self.case["acc"]
            telemetry = (
                [
                    (
                        "FLEX|" + "|".join(
                            f"{name}={value}"
                            for name, value in zip(RUNNER.FLEX_KEYS, flex)
                        ) + "\r\n"
                    ).encode("ascii"),
                    f"IMU|R={imu['roll']:.2f}|P={imu['pitch']:.2f}|Y={imu['yaw']:.2f}\r\n".encode("ascii"),
                    f"ACC|X={acc['x']:.3f}|Y={acc['y']:.3f}|Z={acc['z']:.3f}|VALID={1 if acc['valid'] else 0}\r\n".encode("ascii"),
                ]
            )
            if self.telemetry_after_apply is None:
                self.lines.extend(telemetry)
            else:
                self.lines.extend(
                    (line + "\r\n").encode("ascii")
                    for line in self.telemetry_after_apply
                )
        return len(payload)

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class RunnerTests(unittest.TestCase):
    def run_with_telemetry(self, lines, timeout=0.02):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case, telemetry_after_apply=lines)
        RUNNER.run_virtual_test(
            serial, case, timeout=timeout, settle_seconds=0,
            enter_timeout=0.01, exit_timeout=0.01,
        )
        self.assertEqual(serial.writes[-1], "TEST:EXIT")
        return case

    @staticmethod
    def correct_frames(case):
        flex = "FLEX|" + "|".join(
            f"{key}={value}" for key, value in zip(RUNNER.FLEX_KEYS, case["flex"])
        )
        imu = case["imu"]
        acc = case["acc"]
        return (
            flex,
            f"IMU|R={imu['roll']:.2f}|P={imu['pitch']:.2f}|Y={imu['yaw']:.2f}",
            f"ACC|X={acc['x']:.3f}|Y={acc['y']:.3f}|Z={acc['z']:.3f}|VALID={int(acc['valid'])}",
        )

    def test_all_case_files_are_valid(self):
        for path in sorted((ROOT / "tests" / "cases").glob("*.json")):
            with self.subTest(path=path.name):
                RUNNER.validate_case(json.loads(path.read_text(encoding="utf-8")))

    def test_invalid_case_rejected(self):
        with self.assertRaises(ValueError):
            RUNNER.validate_case(
                {
                    "flex": [0] * 9 + [101],
                    "imu": {"roll": 0, "pitch": 0, "yaw": 0},
                    "acc": {"x": 0, "y": 0, "z": 1, "valid": True},
                }
            )

    def test_fake_serial_full_flow(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case)
        RUNNER.run_virtual_test(
            serial, case, timeout=0.01, settle_seconds=0,
            enter_timeout=0.01, exit_timeout=0.01,
        )
        self.assertEqual(serial.writes[0], "TEST:ENTER")
        self.assertEqual(serial.writes[-1], "TEST:EXIT")

    def test_enter_retries_timeout_then_continues_full_flow(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case, miss_responses={"TEST:ENTER": 1})
        RUNNER.run_virtual_test(
            serial, case, timeout=0.01, settle_seconds=0,
            enter_timeout=0.005, exit_timeout=0.005,
        )
        self.assertEqual(
            [line for line in serial.writes if line == "TEST:ENTER"],
            ["TEST:ENTER", "TEST:ENTER"],
        )
        self.assertTrue(any(line.startswith("TEST:FLEX|") for line in serial.writes))
        self.assertIn("TEST:APPLY", serial.writes)

    def test_enter_all_attempts_fail_and_no_case_commands_are_sent(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case, miss_responses={"TEST:ENTER": 3})
        with self.assertRaisesRegex(RUNNER.TestFailure, "MODE=VIRTUAL"):
            RUNNER.run_virtual_test(
                serial, case, timeout=0.01, settle_seconds=0,
                enter_attempts=3, enter_timeout=0.003,
                exit_attempts=1, exit_timeout=0.003,
            )
        self.assertEqual(
            [line for line in serial.writes if line == "TEST:ENTER"],
            ["TEST:ENTER"] * 3,
        )
        self.assertFalse(any(line.startswith("TEST:FLEX|") for line in serial.writes))
        self.assertNotIn("TEST:APPLY", serial.writes)
        self.assertEqual(serial.writes[-1], "TEST:EXIT")

    def test_exit_retries_once_and_then_confirms_real_mode(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case, miss_responses={"TEST:EXIT": 1})
        RUNNER.run_virtual_test(
            serial, case, timeout=0.01, settle_seconds=0,
            enter_timeout=0.005, exit_attempts=3, exit_timeout=0.005,
        )
        self.assertEqual(
            [line for line in serial.writes if line == "TEST:EXIT"],
            ["TEST:EXIT", "TEST:EXIT"],
        )

    def test_exit_must_be_confirmed_for_pass(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(
            case, confirm_exit=False, miss_responses={"TEST:EXIT": 3}
        )
        with self.assertRaisesRegex(RUNNER.TestFailure, "TEST:EXIT was not confirmed"):
            RUNNER.run_virtual_test(
                serial, case, timeout=0.01, settle_seconds=0,
                enter_timeout=0.005, exit_attempts=3, exit_timeout=0.003,
            )
        self.assertEqual(
            [line for line in serial.writes if line == "TEST:EXIT"],
            ["TEST:EXIT"] * 3,
        )

    def test_old_imu_and_acc_then_new_frames_converge(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        flex, imu, acc = self.correct_frames(case)
        self.run_with_telemetry([
            flex, "IMU|R=0.00|P=0.00|Y=0.00",
            "ACC|X=0.000|Y=0.000|Z=0.000|VALID=1", imu, acc,
        ])

    def test_imu_never_converges_reports_latest_values(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        flex, _, acc = self.correct_frames(case)
        with self.assertRaises(RUNNER.TestFailure) as failure:
            self.run_with_telemetry([
                flex, "IMU|R=0.00|P=0.00|Y=0.00", acc,
                "IMU|R=0.00|P=0.00|Y=0.00",
            ])
        message = str(failure.exception)
        for label in ("expected FLEX", "latest FLEX", "expected IMU",
                      "latest IMU", "expected ACC", "latest ACC"):
            self.assertIn(label, message)
        self.assertIn("[0.0, 0.0, 0.0]", message)

    def test_old_flex_then_new_flex_converges(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        flex, imu, acc = self.correct_frames(case)
        self.run_with_telemetry([
            "FLEX|" + "|".join(f"{key}=0" for key in RUNNER.FLEX_KEYS),
            imu, acc, flex,
        ])

    def test_telemetry_converges_in_any_order(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        flex, imu, acc = self.correct_frames(case)
        for order in ([acc, flex, imu], [imu, acc, flex], [flex, imu, acc]):
            with self.subTest(order=order):
                self.run_with_telemetry(order)


if __name__ == "__main__":
    unittest.main()
