"""Load telemetry checks must reject a silently inactive stress generator."""
import unittest
from unittest import mock
with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
    import run_s3_load as runner


class LoadRunnerTests(unittest.TestCase):
    def output_stats(self):
        return dict(lastBreakUs=176, lastMabUs=16, fps=40,
                    breakUs=dict(n=10, min=170), mabUs=dict(n=10, min=14))

    def test_valid_output_timing(self):
        runner.verify_output_timing(self.output_stats())

    def test_intermittent_completed_short_mab_is_not_hidden(self):
        stats = self.output_stats()
        stats["mabUs"]["min"] = 7
        with self.assertRaisesRegex(AssertionError, "Completed output"):
            runner.verify_output_timing(stats)

    def test_short_live_mab_is_rejected(self):
        stats = self.output_stats()
        stats["lastMabUs"] = 7
        with self.assertRaisesRegex(AssertionError, "Sampled output"):
            runner.verify_output_timing(stats)

    def test_missing_completed_timing_is_rejected(self):
        stats = self.output_stats()
        stats["breakUs"]["n"] = 0
        with self.assertRaisesRegex(AssertionError, "No completed"):
            runner.verify_output_timing(stats)

    def test_completed_short_break_is_rejected(self):
        stats = self.output_stats()
        stats["breakUs"]["min"] = 87
        with self.assertRaisesRegex(AssertionError, "Completed output"):
            runner.verify_output_timing(stats)

    def telemetry(self):
        before = dict(load="combined", busyUs=[0, 0], loadCycles=[0, 0], wifiMode=3,
                      scansCompleted=0, scanFailures=0, txTimeouts=0, freeHeap=100000)
        after = dict(before, busyUs=[21000000, 21000000], loadCycles=[3000, 3000], scansCompleted=10)
        return before, after

    def test_combined_load_measured_on_both_cores(self):
        before, after = self.telemetry()
        result = runner.verify_load(before, after, "combined", 30)
        self.assertEqual(result["measuredBusyFraction"], [0.7, 0.7])

    def test_inactive_worker_rejected(self):
        before, after = self.telemetry()
        after["busyUs"][1] = 0
        with self.assertRaisesRegex(AssertionError, "both cores"):
            runner.verify_load(before, after, "combined", 30)

    def test_inactive_wifi_rejected(self):
        before, after = self.telemetry()
        after["scansCompleted"] = 0
        with self.assertRaisesRegex(AssertionError, "scans"):
            runner.verify_load(before, after, "combined", 30)

    def test_transmit_timeout_rejected(self):
        before, after = self.telemetry()
        after["txTimeouts"] = 1
        with self.assertRaisesRegex(AssertionError, "timeout"):
            runner.verify_load(before, after, "combined", 30)

    def test_wrapping_telemetry_counter(self):
        self.assertEqual(runner.delta(5, 0xFFFFFFFE), 7)

    def test_missing_load_firmware_rejected_and_cleanup_attempted(self):
        bench = mock.Mock()
        bench.command.return_value = {}
        with self.assertRaisesRegex(AssertionError, "firmware"):
            runner.run_cases(mock.Mock(), bench, {"tests": {}})
        self.assertEqual(bench.command.call_args_list,
                         [mock.call("load combined"), mock.call("load none")])

    def test_large_heap_decline_rejected(self):
        before, after = self.telemetry()
        after["freeHeap"] -= 10000
        with self.assertRaisesRegex(AssertionError, "heap"):
            runner.verify_load(before, after, "combined", 30)


if __name__ == "__main__":
    unittest.main()
