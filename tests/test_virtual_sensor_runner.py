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
    def __init__(self, case, confirm_exit=True):
        self.case = case
        self.confirm_exit = confirm_exit
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
        if key != "TEST:EXIT" or self.confirm_exit:
            self.lines.append((replies[key] + "\r\n").encode("ascii"))
        if key == "TEST:APPLY":
            flex = self.case["flex"]
            imu = self.case["imu"]
            acc = self.case["acc"]
            self.lines.extend(
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
        return len(payload)

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class RunnerTests(unittest.TestCase):
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
        RUNNER.run_virtual_test(serial, case, timeout=0.1)
        self.assertEqual(serial.writes[0], "TEST:ENTER")
        self.assertEqual(serial.writes[-1], "TEST:EXIT")

    def test_exit_must_be_confirmed_for_pass(self):
        case = RUNNER.load_case(ROOT / "tests" / "cases" / "mixed.json")
        serial = FakeSerial(case, confirm_exit=False)
        with self.assertRaisesRegex(RUNNER.TestFailure, "TEST:EXIT was not confirmed"):
            RUNNER.run_virtual_test(serial, case, timeout=0.01)


if __name__ == "__main__":
    unittest.main()
