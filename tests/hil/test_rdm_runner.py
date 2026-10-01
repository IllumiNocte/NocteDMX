"""Exercise the S3 RDM host matrix without serial devices or programming."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock


def load_runner():
    spec = importlib.util.spec_from_file_location("nocte_s3_rdm", Path(__file__).with_name("run_s3_rdm.py"))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
        spec.loader.exec_module(module)
    return module


class SyntheticFixture:
    def __init__(self):
        self.profile = "conformant"
        self.commands = []

    def idle(self):
        return {"ok": True}

    def command(self, **payload):
        self.commands.append(payload)
        if payload.get("action") == "profile":
            self.profile = payload["name"]
        return {"ok": True}


class SyntheticBench:
    def __init__(self, fixture, physical_short_mab=22, guard=165):
        self.fixture = fixture
        self.address, self.identify = 42, 0
        self.physical_short_mab, self.guard = physical_short_mab, guard
        self.frames = 0

    def command(self, text, kind="status"):
        fields = text.split()
        if text == "invalid":
            return {"statuses": [6] * 6}
        if text == "burst":
            return {"acked": 100, "flags": 0, "beforeHeap": 10000, "afterHeap": 10000}
        if fields[0] not in ("get", "set"):
            self.frames += 1
            return {"freeHeap": 10000, "txFrames": self.frames, "txTimeouts": 0}
        profile = self.fixture.profile
        if profile == "short-mab" and self.physical_short_mab < 8:
            return {"status": 5, "flags": 4096, "copied": 0,
                    "timing": {"mabUs": self.physical_short_mab}}
        if profile not in ("conformant", "earliest-valid", "latest-valid",
                            "slow-frame-valid", "short-break", "short-mab"):
            return {"status": 5, "copied": 0}
        pid = int(fields[1], 16)
        reply = {"status": 0, "flags": 0, "guard": self.guard,
                 "timing": {"mabUs": self.physical_short_mab}}
        if pid == 0xFFFE:
            reply.update(status=1, pdl=2, copied=0)
        elif fields[0] == "set":
            if pid == 0xF0:
                self.address = int(fields[2], 16)
            elif pid == 0x1000:
                self.identify = int(fields[2], 16)
            reply.update(pdl=0, copied=0)
        else:
            if pid == 0x60:
                data = [0] * 19
                data[11], data[15] = 16, self.address
            elif pid == 0xF0:
                data = [0, self.address]
            else:
                data = [self.identify]
            capacity = int(fields[2])
            reply.update(pdl=len(data), copied=min(capacity, len(data)), data=data[:capacity])
        return reply


class RdmRunnerDryTests(unittest.TestCase):
    def setUp(self):
        self.runner = load_runner()

    def run_matrix(self, mab=22, guard=165):
        fixture = SyntheticFixture()
        report = {"tests": {}}
        with mock.patch.object(self.runner.time, "sleep"), mock.patch("builtins.print"):
            self.runner.run_cases(fixture, SyntheticBench(fixture, mab, guard), report)
        return fixture, report

    def test_matrix_recovers_and_records_fixture_gap(self):
        fixture, report = self.run_matrix()
        self.assertEqual(len(report["tests"]), 38)
        self.assertEqual(sum(key.startswith("reject_and_recover_") for key in report["tests"]), 20)
        self.assertIn("probe_short_mab_fixture_limit", report["tests"])
        self.assertEqual(len(report["coverage_gaps"]), 1)
        self.assertIn({"cmd": "rdm", "action": "fault", "normalBreakUs": 120}, fixture.commands)
        self.assertIn({"cmd": "rdm", "action": "fault", "normalBreakUs": 500}, fixture.commands)

    def test_actual_short_mab_is_rejected_not_reported_as_gap(self):
        _, report = self.run_matrix(mab=7)
        self.assertIn("reject_short_mab", report["tests"])
        self.assertNotIn("coverage_gaps", report)

    def test_copy_guard_failure_stops_matrix(self):
        with self.assertRaisesRegex(AssertionError, "guard"):
            self.run_matrix(guard=0)

    def test_same_port_rejected_before_hardware_access(self):
        with mock.patch("sys.argv", ["runner", "--esp-port", "COM6", "--fixture-port", "com6"]), \
                mock.patch.object(self.runner, "Fixture") as fixture, \
                mock.patch("sys.stderr"):
            with self.assertRaises(SystemExit):
                self.runner.main()
            fixture.assert_not_called()

    def test_long_usb_command_is_paced(self):
        bench = self.runner.Bench.__new__(self.runner.Bench)
        bench.serial = mock.Mock()
        bench.serial.readline.return_value = b'{"type":"status","error":0}\n'
        with mock.patch.object(self.runner.time, "sleep") as sleep:
            bench.command("x" * 474)
        writes = [call.args[0] for call in bench.serial.write.call_args_list]
        self.assertEqual(b"".join(writes), b"x" * 474 + b"\n")
        self.assertTrue(all(len(chunk) <= 48 for chunk in writes))
        self.assertEqual(sleep.call_count, len(writes))


if __name__ == "__main__":
    unittest.main()
