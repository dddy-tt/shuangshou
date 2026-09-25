from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "tools" / "glove_test" / "test_real_imu.py"
SPEC = importlib.util.spec_from_file_location("real_imu_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


def debug_line(*, online=1, err=0, last=0, acc_raw="0,0,2048",
               gyro_raw="0,0,0", angle_raw="100,0,100", av=1,
               angv=1, aseen=1, gseen=1, tseen=1, aage=10,
               gage=10, tage=10, reads=0, i2cerr="0,0,0",
               halerr="0,0,0,0",
               zero=0, rec="0,0,0"):
    return (
        f"[JYDBG] ONLINE={online}|ERR={err}|LAST={last}|ACC_RAW={acc_raw}|"
        f"GYRO_RAW={gyro_raw}|ANGLE_RAW={angle_raw}|AV={av}|ANGV={angv}|"
        f"ASEEN={aseen}|GSEEN={gseen}|TSEEN={tseen}|AAGE={aage}|"
        f"GAGE={gage}|TAGE={tage}|READS={reads}|I2CERR={i2cerr}|"
        f"HALERR={halerr}|ZERO={zero}|REC={rec}"
    )


def healthy_capture(duration=15.0, *, initial_i2cerr=(0, 0, 0),
                    initial_rec=(0, 0, 0)):
    capture = RUNNER.Capture()
    for t in (0.0, 5.0, 10.0):
        capture.add_line("BRINGUP: JY=1,JY_RET=0,ADC1=1,ADC2=1,BEEP=1", t)
    for second in range(15):
        t = float(second)
        capture.add_line("JY|ONLINE=1|ERR=0|LAST=0|AGE=10", t)
        capture.add_line("ACC|X=0.000|Y=0.000|Z=1.000|VALID=1", t + 0.02)
        capture.add_line(
            debug_line(reads=second * 100,
                       i2cerr=",".join(map(str, initial_i2cerr)),
                       rec=",".join(map(str, initial_rec))),
            t + 0.03,
        )
    for tick in range(60):
        t = tick * 0.25 + 0.05
        capture.add_line("IMU|R=0.00|P=0.00|Y=0.00", t)
    for t in (0.2, 3.2, 6.2, 9.2, 12.2):
        capture.add_line("[STACK]|SIZE=4096|USED=1024|FREE=3072|GUARD=1", t)
    return capture


class RealImuParserTests(unittest.TestCase):
    def test_parses_all_real_sensor_frames_and_unsigned_counters(self):
        capture = RUNNER.Capture()
        self.assertEqual(capture.add_line("BRINGUP: JY=1,JY_RET=0,ADC1=1", 0), "BRINGUP")
        self.assertEqual(capture.add_line("JY|ONLINE=1|ERR=0|LAST=0|AGE=10", 0.1), "JY")
        self.assertEqual(
            capture.add_line(debug_line(i2cerr="40000,0,0", halerr="4,0,0,1",
                                        rec="4294967295,0,0"), 0.2),
            "JYDBG",
        )
        self.assertEqual(capture.debug[0][1]["I2CERR"], (40000, 0, 0))
        self.assertEqual(capture.debug[0][1]["HALERR"], (4, 0, 0, 1))
        self.assertEqual(capture.debug[0][1]["REC"], (0xFFFFFFFF, 0, 0))
        self.assertEqual(capture.add_line("IMU|R=-2.50|P=1.00|Y=0.00", 0.3), "IMU")
        self.assertEqual(capture.add_line("ACC|X=0|Y=0|Z=1|VALID=1", 0.4), "ACC")
        self.assertEqual(capture.add_line("[STACK]|SIZE=4096|USED=1000|FREE=3096|GUARD=1", 0.5), "STACK")
        self.assertEqual(len(capture.malformed), 0)

    def test_static_healthy_window_passes(self):
        report = RUNNER.assess(healthy_capture(), 15.0, True)
        self.assertTrue(all(report["checks"].values()), report["checks"])
        self.assertAlmostEqual(report["metrics"]["online_ratio"], 1.0)
        self.assertAlmostEqual(report["metrics"]["read_rate_hz"], 100.0)
        self.assertEqual(report["metrics"]["acc_raw_formal_mismatches"], 0)

    def test_one_angle_zero_is_transient_not_an_i2c_failure(self):
        capture = healthy_capture()
        capture.add_line(debug_line(last=0x10, angv=0, zero=1, reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertTrue(report["checks"]["I2C errors/recovery stable"])
        self.assertTrue(report["checks"]["ACC/GYRO/ANGLE samples seen"])
        self.assertTrue(report["checks"]["Angle valid ratio >= 80%"])

    def test_illegal_error_mask_and_corruption_fail(self):
        capture = healthy_capture()
        capture.add_line(debug_line(last=61, err=254, reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["Legal status/mask values"])

        capture = healthy_capture()
        capture.add_line(debug_line(online=0, err=4, last=8, reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertTrue(report["checks"]["Legal status/mask values"])

    def test_bus_error_or_recovery_storm_fails(self):
        capture = healthy_capture()
        capture.add_line(debug_line(i2cerr="4,0,0", rec="2,2,0", reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["I2C errors/recovery stable"])

    def test_errors_before_first_window_sample_are_not_counted_as_window_errors(self):
        capture = healthy_capture(initial_i2cerr=(120, 300, 80),
                                  initial_rec=(30, 30, 0))
        report = RUNNER.assess(capture, 15.0, True)
        self.assertTrue(report["checks"]["I2C errors/recovery stable"])
        self.assertEqual(report["metrics"]["i2c_error_counts_delta_during_window"],
                         (0, 0, 0))

    def test_decreasing_cumulative_counters_fail_as_unreliable_window(self):
        capture = healthy_capture(initial_i2cerr=(120, 300, 80),
                                  initial_rec=(30, 30, 0))
        capture.add_line(debug_line(i2cerr="0,0,0", rec="0,0,0", reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["I2C errors/recovery stable"])

    def test_age_sentinel_after_sample_seen_fails(self):
        capture = healthy_capture()
        capture.add_line(debug_line(aage=0xFFFFFFFF, reads=1500), 14.1)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["Freshness <= 120 ms"])

    def test_raw_to_formal_acc_mismatch_fails_scale_check(self):
        capture = healthy_capture()
        capture.add_line("ACC|X=0.500|Y=0.000|Z=1.000|VALID=1", 14.02)
        capture.add_line(debug_line(reads=1500), 14.03)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["ACC raw/formal scale agrees"])

    def test_virtual_mode_or_active_alarm_is_rejected(self):
        capture = healthy_capture()
        capture.add_line("[TEST] MODE=VIRTUAL", 14.0)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["Mode REAL confirmed"])

        capture = healthy_capture()
        capture.add_line("ALARM|BOOT=1|ID=2|TYPE=1|ACTIVE=1", 14.0)
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["No active alarm"])

    def test_malformed_or_nonfinite_sensor_frames_are_recorded(self):
        capture = healthy_capture()
        self.assertEqual(capture.add_line("IMU|R=nan|P=0|Y=0", 14.0), "MALFORMED")
        report = RUNNER.assess(capture, 15.0, True)
        self.assertFalse(report["checks"]["No malformed sensor frames"])

    def test_capture_must_confirm_real_and_meet_sample_counts(self):
        capture = healthy_capture()
        report = RUNNER.assess(capture, 15.0, False)
        self.assertFalse(report["checks"]["Mode REAL confirmed"])
        empty = RUNNER.assess(RUNNER.Capture(), 15.0, True)
        self.assertFalse(empty["checks"]["Bringup JY=1/JY_RET=0"])
        self.assertFalse(empty["checks"]["JY online ratio >= 90%"])
        self.assertFalse(empty["checks"]["ACC/GYRO/ANGLE samples seen"])

    def test_real_mode_handshake_sends_exactly_one_exit_and_no_virtual_commands(self):
        class Port:
            def __init__(self):
                self.writes = []
                self.responses = [b"[TEST] MODE=REAL\r\n"]

            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.writes.append(data)
                return len(data)

            def flush(self):
                pass

            def readline(self):
                return self.responses.pop(0) if self.responses else b""

        from io import StringIO
        port = Port()
        log = StringIO()
        RUNNER._establish_real_mode(port, log, RUNNER.Capture(), 0.0)
        self.assertEqual(port.writes, [b"TEST:EXIT\n"])
        self.assertIn("TX TEST:EXIT", log.getvalue())
        self.assertNotIn("TEST:ENTER", log.getvalue())
        self.assertNotIn("TEST:IMU", log.getvalue())


if __name__ == "__main__":
    unittest.main()
