"""Reject missing/inactive load and bad payload or raw timing observations."""
import unittest
from unittest import mock
with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
    import run_esp8266_load as runner


class Esp8266LoadTests(unittest.TestCase):
    def frame(self):
        values = [0] * 64
        values[:6] = [0xA5, 0xD6, 1, 0, 1, 1]
        values[7], values[11] = 4, 80
        return dict(slots=512, values=values)

    def raw(self):
        return dict(method="pio_dma_raw", valid=True, stalled=False,
                    timedOut=False, words=16384, sampleUs=.25,
                    breakUs=dict(n=4, min=102), mabUs=dict(n=4, min=16), shortMabs=0)

    def test_valid_telemetry(self):
        self.assertEqual(runner.telemetry(self.frame(), True)["cpuMHz"], 80)

    def test_wrong_mode(self):
        with self.assertRaisesRegex(AssertionError, "mode"):
            runner.telemetry(self.frame(), False)

    def test_truncated_payload(self):
        frame = self.frame()
        frame["slots"] = 511
        with self.assertRaisesRegex(AssertionError, "telemetry"):
            runner.telemetry(frame, True)

    def test_cumulative_failure_cannot_hide_between_snapshots(self):
        for index in (19, 39, 47, 51, 63):
            frame = self.frame()
            frame["values"][index] = 1
            with self.subTest(index=index), self.assertRaisesRegex(AssertionError, "failures"):
                runner.telemetry(frame, True)

    def test_wifi_mode_must_match_profile(self):
        frame = self.frame()
        frame["values"][3] = 2
        with self.assertRaisesRegex(AssertionError, "Wi-Fi"):
            runner.telemetry(frame, True)

    def test_valid_raw_capture(self):
        runner.verify_raw(self.raw())

    def test_short_mab(self):
        raw = self.raw()
        raw["mabUs"]["min"] = 7.75
        with self.assertRaisesRegex(AssertionError, "limits"):
            runner.verify_raw(raw)

    def test_stalled_capture(self):
        raw = self.raw()
        raw["stalled"] = True
        with self.assertRaisesRegex(AssertionError, "Invalid"):
            runner.verify_raw(raw)

    def test_missing_pairs(self):
        raw = self.raw()
        raw["breakUs"]["n"] = 0
        with self.assertRaisesRegex(AssertionError, "pulses"):
            runner.verify_raw(raw)

    def test_frozen_load_rejected(self):
        samples = []
        for profile in range(4):
            for cycle in range(3):
                sample = runner.telemetry(self.frame(), True)
                sample.update(profile=profile, cycle=cycle, goodGets=cycle * 5,
                              scanAttempts=cycle, scansCompleted=cycle, cpuWindows=cycle)
                samples.append(sample)
        with self.assertRaisesRegex(AssertionError, "load did not progress"):
            runner.validate_progress(samples, True)


if __name__ == "__main__":
    unittest.main()
