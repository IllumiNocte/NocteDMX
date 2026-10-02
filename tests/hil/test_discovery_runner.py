"""Hardware-free assertions for the discovery matrix; no serial dependency."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock


def load_runner():
    spec = importlib.util.spec_from_file_location("nocte_discovery", Path(__file__).with_name("run_s3_discovery.py"))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
        spec.loader.exec_module(module)
    return module


class Fixture:
    profile = "conformant"

    def idle(self):
        pass

    def command(self, **payload):
        if payload.get("cmd") == "ping":
            return {"ok": True, "fw": "0.4.17"}
        if payload.get("action") == "profile":
            self.profile = payload["name"]
        return {"ok": True}


class Bench:
    def __init__(self, fixture):
        self.fixture, self.muted = fixture, False
        self.scans = 0
        self.accept_fault = False
        self.early_is_legal = False
        self.frames = 1

    def command(self, text, kind="status", timeout=5):
        self.frames += 1
        fields = text.split()
        if fields[0] in ("mute", "unmute"):
            self.muted = fields[0] == "mute"
            return {"status": 7 if fields[1].endswith("FFFFFFFF") else 0, "flags": 0, "pdl": 2}
        if fields[0] == "branch":
            result = 0 if self.muted or self.fixture.profile == "dropped" else 2
            if self.fixture.profile not in ("conformant", "dropped") and not self.accept_fault:
                result = 1
            if self.fixture.profile == "too-early" and self.early_is_legal:
                result = 2
            if fields[1] > fields[2]:
                result = 1
            elif fields[2] == "000000000001":
                result = 0
            return {"result": result, "uid": "7FF0:52444D01", "length": 24,
                    "spacingUs": 500, "packetUs": 1056}
        if fields[0] == "scan":
            self.scans += 1
            limit = int(fields[1]) if len(fields) > 1 else 512
            return {"status": 2 if limit < 6 else 0, "transactions": limit if limit < 6 else 4,
                    "unresolved": 0, "uids": ["7FF0:52444D01"],
                    "beforeHeap": 10000, "afterHeap": 10000}
        if fields[0] == "get":
            return {"status": 0, "flags": 0}
        return {"txTimeouts": 0, "txFrames": self.frames, "freeHeap": 10000}


class DiscoveryRunnerTests(unittest.TestCase):
    def test_matrix_checks_budgets_faults_and_repeatability(self):
        runner = load_runner()
        fixture = Fixture()
        bench = Bench(fixture)
        report = {"tests": {}}
        with mock.patch.object(runner.time, "sleep"), mock.patch("builtins.print"):
            runner.run_cases(fixture, bench, report)
        self.assertEqual(len(report["tests"]), 24)
        self.assertEqual(bench.scans, 23)
        self.assertIn("get_while_muted", report["tests"])
        self.assertIn("malformed_scan_budget", report["tests"])
        self.assertIn("coverage_gaps", report)

    def test_accepted_malformed_reply_fails_matrix(self):
        runner = load_runner()
        fixture = Fixture()
        bench = Bench(fixture)
        bench.accept_fault = True
        with mock.patch.object(runner.time, "sleep"), mock.patch("builtins.print"):
            with self.assertRaisesRegex(AssertionError, "Fault accepted"):
                runner.run_cases(fixture, bench, {"tests": {}})

    def test_fixture_legal_early_probe_records_gap_not_rejection(self):
        runner = load_runner()
        fixture = Fixture()
        bench = Bench(fixture)
        bench.early_is_legal = True
        report = {"tests": {}}
        with mock.patch.object(runner.time, "sleep"), mock.patch("builtins.print"):
            runner.run_cases(fixture, bench, report)
        self.assertIn("probe_early_discovery_fixture_limit", report["tests"])
        self.assertNotIn("reject_and_recover_too-early", report["tests"])
        self.assertEqual(len(report["coverage_gaps"]), 2)


if __name__ == "__main__":
    unittest.main()
