"""S3 direct-UART discovery, mute/unmute and bounded full-scan HIL.

One RP2040 responder; electrical multi-device collisions require RS485 hardware.
The native core suite supplies deterministic multi-device collision coverage.
"""
import sys
import time
from run_smoke import require
from run_s3_rdm import main as run_harness
from run_s3_bench import heap_tolerance


def run_cases(fixture, bench, report):
    version = fixture.command(cmd="ping").get("fw", "")
    require(tuple(int(part) for part in version.split(".")) >= (0, 4, 17),
            "Discovery HIL needs tester 0.4.17 or later (edge/early-discovery fixes)")
    uid = "7FF0:52444D01"
    raw_uid = uid.replace(":", "")

    def record(name, reply):
        report["tests"][name] = reply
        print("PASS " + name, flush=True)

    def profile(name):
        fixture.command(cmd="rdm", action="profile", name=name)
        time.sleep(0.03)

    def branch(low="000000000000", high="FFFFFFFFFFFF"):
        return bench.command("branch " + low + " " + high, "discovery")

    def mute(value, target=raw_uid, status=0):
        reply = bench.command(("mute " if value else "unmute ") + target, "mute")
        require(reply["status"] == status and reply["flags"] == 0, "Mute failed: " + repr(reply))
        if status == 0:
            require(reply["pdl"] in (2, 8), "Invalid control-field length")
        return reply

    fixture.idle()
    fixture.command(cmd="rdm", action="defaults")
    fixture.command(cmd="rdm", action="configure", uid=uid, responseDelayUs=500)
    profile("conformant")
    fixture.command(cmd="mode", value="rdm")
    bench.command("rdm")
    record("broadcast_unmute", mute(False, "FFFFFFFFFFFF", 7))
    for name, low, high in (("full_range", "000000000000", "FFFFFFFFFFFF"),
                            ("exact_uid", raw_uid, raw_uid)):
        reply = branch(low, high)
        require(reply["result"] == 2 and reply["uid"] == uid and reply["length"] == 24,
                "Discovery did not decode fixture: " + repr(reply))
        require(176 <= reply["spacingUs"] <= 2800 and reply["packetUs"] <= 2900,
                "Discovery timing outside limits: " + repr(reply))
        record(name, reply)
    for name, low, high in (("excluded_range", "000000000000", "000000000001"),
                           ("invalid_range", "FFFFFFFFFFFF", "000000000000")):
        reply = branch(low, high)
        require(reply["result"] == (1 if name == "invalid_range" else 0), repr(reply))
        record(name, reply)
    record("unicast_mute", mute(True))
    reply = branch()
    require(reply["result"] == 0, "Muted fixture still replied")
    record("muted_no_discovery", reply)
    details = bench.command("get 60", "rdm")
    require(details["status"] == 0 and details["flags"] == 0, "Mute must not affect GET")
    record("get_while_muted", details)
    record("unicast_unmute", mute(False))
    record("manufacturer_mute", mute(True, "7FF0FFFFFFFF", 7))
    require(branch()["result"] == 0, "Manufacturer broadcast mute was ignored")
    record("manufacturer_unmute", mute(False, "7FF0FFFFFFFF", 7))
    require(branch()["result"] == 2, "Manufacturer broadcast unmute was ignored")
    record("broadcast_mute", mute(True, "FFFFFFFFFFFF", 7))
    require(branch()["result"] == 0, "Broadcast mute was ignored")
    mute(False, "FFFFFFFFFFFF", 7)
    for name in ("bad-checksum", "truncated", "extra-byte", "too-early", "too-late"):
        profile(name)
        reply = branch()
        if name == "too-early" and reply["result"] == 2 and 176 <= reply["spacingUs"] <= 2800:
            require(reply["uid"] == uid and reply["packetUs"] <= 2900, repr(reply))
            report.setdefault("coverage_gaps", []).append(
                "RP2040 discovery delay=0 still produced legal SOP spacing; sub-176-us rejection is native-tested only")
            time.sleep(0.05)
            profile("conformant")
            recovery = branch()
            require(recovery["result"] == 2 and recovery["uid"] == uid, "No recovery after " + name)
            record("probe_early_discovery_fixture_limit", {"probe": reply, "recovery": recovery})
            continue
        require(reply["result"] == 1, "Fault accepted as valid/empty: " + name + " " + repr(reply))
        time.sleep(0.05)
        profile("conformant")
        recovery = branch()
        require(recovery["result"] == 2 and recovery["uid"] == uid, "No recovery after " + name)
        record("reject_and_recover_" + name, {"fault": reply, "recovery": recovery})
    # A dropped answer really has no bus activity, unlike malformed replies.
    profile("dropped")
    empty = branch()
    require(empty["result"] == 0, "Dropped reply was not empty")
    record("dropped_response", empty)
    profile("conformant")
    limited = bench.command("scan 2", "scan", timeout=60)
    require(limited["status"] == 2 and limited["transactions"] == 2, "Scan ignored budget")
    record("scan_budget", limited)
    baseline = bench.command("status")
    for index in range(20):
        scan = bench.command("scan", "scan", timeout=60)
        require(scan["status"] == 0 and scan["uids"] == [uid] and scan["unresolved"] == 0,
                "Full scan failed: " + repr(scan))
        report.setdefault("scan_heap_observations", []).append(scan)
        require(scan["afterHeap"] >= scan["beforeHeap"] - heap_tolerance(report), "Heap decreased during scan")
        if index == 0:
            record("full_scan", scan)
    after = bench.command("status")
    require(after["txFrames"] >= baseline["txFrames"] + 20, "DMX did not resume between scans")
    require(after["freeHeap"] >= baseline["freeHeap"] - heap_tolerance(report), "Heap fell across repeated scans")
    record("20_repeated_scans", {"last_scan": scan, "before": baseline, "after": after})
    profile("bad-checksum")
    limited = bench.command("scan 5", "scan", timeout=60)
    require(limited["status"] == 2 and limited["transactions"] == 5, "Malformed scan ignored budget")
    record("malformed_scan_budget", limited)
    profile("conformant")
    final = bench.command("scan", "scan", timeout=60)
    require(final["status"] == 0 and final["uids"] == [uid], "Final recovery failed")
    record("final_scan_recovery", final)
    status = bench.command("status")
    require(status["txTimeouts"] == 0 and status["txFrames"] > 0, "DMX did not resume")
    record("dmx_resumes", status)
    report["fixture_after"] = fixture.command(cmd="rdm", action="status")
    report.setdefault("coverage_gaps", []).append(
        "No electrical RS485 collisions or DE/RE timing measured; direct UART only")


if __name__ == "__main__":
    sys.exit(run_harness(run_cases, "build/hil/s3-discovery-report.json"))
