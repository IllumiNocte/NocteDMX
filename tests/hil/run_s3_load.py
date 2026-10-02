"""S3 UART load qualification: CPU on both cores and Wi-Fi AP/active scans.

No station credentials or external traffic generator required. This is not a
sustained UDP/network throughput test or calibrated RS485 timing measurement.
"""
import sys
import time

from run_smoke import require
from run_s3_rdm import main as run_harness, run_cases as rdm_matrix
from run_s3_bench import run_cases as dmx_matrix
from run_s3_discovery import run_cases as discovery_matrix


def delta(after, before):
    return (after - before) & 0xFFFFFFFF


def verify_output_timing(stats):
    require(stats["lastBreakUs"] >= 88 and stats["lastMabUs"] >= 8,
            "Sampled output timing below DMX limits: " + repr(stats))
    # Live last-frame fields can already describe the next frame. Also inspect
    # completed-frame minima so an intermittent violation between host samples
    # cannot disappear when the next live frame has good timing.
    for field, minimum in (("breakUs", 88), ("mabUs", 8)):
        require(stats[field]["n"] > 0, "No completed output timing samples")
        require(stats[field]["min"] >= minimum,
                "Completed output timing below DMX limits: " + repr(stats))
    require(30 <= stats["fps"] <= 45, "Output refresh fell under load: " + repr(stats))


def verify_load(before, after, profile, seconds):
    require(after.get("load") == profile, "Load profile not active: " + repr(after))
    result = {"profile": profile, "durationSeconds": seconds, "before": before, "after": after}
    if profile in ("cpu", "combined"):
        duty = [delta(after["busyUs"][i], before["busyUs"][i]) / (seconds * 1e6)
                for i in range(2)]
        require(all(0.40 <= value <= 1.05 for value in duty),
                "CPU workers did not sustain load on both cores: " + repr(duty))
        require(all(delta(after["loadCycles"][i], before["loadCycles"][i]) > 100
                    for i in range(2)), "CPU worker stopped making progress")
        result["measuredBusyFraction"] = duty
    if profile in ("wifi", "combined"):
        require(after["wifiMode"] == 3, "Wi-Fi AP+STA mode is not active")
        require(delta(after["scansCompleted"], before["scansCompleted"]) >= 2,
                "Wi-Fi scans did not complete during load")
        require(after["scanFailures"] == before["scanFailures"], "Wi-Fi scan failure")
    require(after["txTimeouts"] == before["txTimeouts"], "New UART TX timeout under load")
    # Active scans temporarily allocate result storage; allow bounded snapshot
    # variation here, and compare quiescent same-state heap after load cleanup.
    require(after["freeHeap"] >= before["freeHeap"] - 8192, "Large heap decline under load")
    return result


def run_cases(fixture, bench, report):
    seconds = 30
    clean = [(i * 17 + 3) & 255 for i in range(512)]
    uid = "7FF0:52444D01"
    failures = []

    def record(name, result):
        report["tests"][name] = result
        print(("FAIL " if result.get("ok") is False else "PASS ") + name, flush=True)

    def expect(condition, message, result, name):
        if not condition:
            result.setdefault("qualityErrors", []).append(message)
            result["ok"] = False
            if name not in failures:
                failures.append(name)

    def loaded_phase(profile, name, step):
        before = bench.command("status")
        start = time.monotonic()
        samples = 0
        errors = []
        while time.monotonic() - start < seconds:
            try:
                step()
            except AssertionError as error:
                errors.append(str(error))
            samples += 1
            time.sleep(0.1)
        elapsed = time.monotonic() - start
        after = bench.command("status")
        result = verify_load(before, after, profile, elapsed)
        result["samples"] = samples
        result["sampleErrors"] = errors
        if errors:
            failures.append(profile + "_" + name)
            result["ok"] = False
            print("FAIL " + failures[-1] + " (%d sample errors)" % len(errors), flush=True)
        return result

    try:
        # Warm both CPU tasks and the Wi-Fi subsystem before measuring leakage.
        require(bench.command("load combined").get("load") == "combined", "Load bench firmware required")
        time.sleep(3)
        bench.command("load none")
        bench.command("stop")
        time.sleep(1)
        baseline = bench.command("status")
        for profile in ("none", "cpu", "wifi", "combined"):
            fixture.idle()
            require(bench.command("load " + profile).get("load") == profile, "Load setup failed")
            time.sleep(2)
            bench.command("input")
            fixture.command(cmd="set", target="timing", breakUs=176, mabUs=16,
                            interSlotUs=0, mbbUs=0, baud=250000, fps=40)
            fixture.command(cmd="set", target="frame", slots=512, values=clean, startCode=0)
            fixture.command(cmd="tx", action="start")
            bench.wait_frame(clean)
            result = loaded_phase(profile, "input_512", lambda: bench.wait_frame(clean, timeout=2))
            expect(result["after"]["rxErrors"] == result["before"]["rxErrors"],
                   "New UART receive errors under clean input load", result, profile + "_input_512")
            frames = delta(result["after"]["rxFrames"], result["before"]["rxFrames"])
            expect(frames >= result["durationSeconds"] * 36, "Input frame rate fell under load",
                   result, profile + "_input_512")
            result["receivedFrames"] = frames
            record(profile + "_input_512", result)

            fixture.idle()
            bench.command("output")
            fixture.command(cmd="mode", value="rx")
            fixture.wait_frame(lambda f: f["slots"] == 512 and f["values"] == clean)
            time.sleep(1.2)  # Allow the fixture's one-second FPS window to settle.
            timing = []
            last_sample = 0

            def output_step():
                nonlocal last_sample
                # Avoid making the GPIO-edge fixture serialize a full JSON
                # frame ten times a second while it is also timing DMX.
                if time.monotonic() - last_sample < 1:
                    return
                last_sample = time.monotonic()
                frame = fixture.frame()
                require(frame["slots"] == 512 and frame["values"] == clean, "Output pattern corrupted")
                stats = fixture.command(cmd="get", target="stats")
                timing.append(stats)
                verify_output_timing(stats)

            result = loaded_phase(profile, "output_512", output_step)
            result["timingSamples"] = timing
            expect(delta(result["after"]["txFrames"], result["before"]["txFrames"])
                   >= result["durationSeconds"] * 36, "S3 output rate fell under load",
                   result, profile + "_output_512")
            record(profile + "_output_512", result)

            fixture.idle()
            fixture.command(cmd="rdm", action="defaults")
            fixture.command(cmd="rdm", action="configure", uid=uid, responseDelayUs=500)
            fixture.command(cmd="rdm", action="profile", name="conformant")
            fixture.command(cmd="mode", value="rdm")
            bench.command("rdm")
            transactions, scans, attempts = 0, 0, 0
            rdm_errors = []

            def rdm_step():
                nonlocal transactions, scans, attempts
                attempts += 1
                read = bench.command("get 60", "rdm")
                if read["status"] == 0 and read["flags"] == 0 and read["pdl"] == 19 \
                        and read["copied"] == 19 and read["guard"] == 165:
                    transactions += 1
                else:
                    rdm_errors.append({"operation": "get_device_info", "reply": read})
                if attempts % 5 == 0:
                    for value in (1, 0):
                        write = bench.command("set 1000 %02x" % value, "rdm")
                        if write["status"] != 0 or write["flags"] != 0:
                            rdm_errors.append({"operation": "set_identify", "reply": write})
                        read = bench.command("get 1000", "rdm")
                        if read["status"] != 0 or read["flags"] != 0 or read["data"] != [value]:
                            rdm_errors.append({"operation": "get_identify", "reply": read})
                    scan = bench.command("scan", "scan", timeout=10)
                    if scan["status"] == 0 and scan["uids"] == [uid] and scan["unresolved"] == 0:
                        scans += 1
                    else:
                        rdm_errors.append({"operation": "scan", "reply": scan})

            result = loaded_phase(profile, "rdm_discovery", rdm_step)
            expect(attempts >= 100, "Too few loaded RDM attempts", result, profile + "_rdm_discovery")
            expect(result["after"]["txFrames"] > result["before"]["txFrames"], "DMX did not resume",
                   result, profile + "_rdm_discovery")
            result.update(deviceInfoGetAttempts=attempts, deviceInfoGets=transactions,
                          identifySetReadbackAttempts=(attempts // 5) * 2, fullScanAttempts=attempts // 5,
                          fullScans=scans, errors=rdm_errors,
                          fixture=fixture.command(cmd="rdm", action="status"))
            if rdm_errors or scans < 20 or result.get("ok") is False:
                name = profile + "_rdm_discovery"
                if name not in failures:
                    failures.append(name)
                result["ok"] = False
                report["tests"][name] = result
                print("FAIL " + name + " (%d protocol errors)" % len(rdm_errors), flush=True)
            else:
                result["ok"] = True
                record(profile + "_rdm_discovery", result)
            bench.command("stop")
            fixture.idle()

        # Repeat all existing valid/fault/recovery matrices at combined load.
        for name, matrix in (("dmx", dmx_matrix), ("rdm", rdm_matrix), ("discovery", discovery_matrix)):
            child = {"tests": {}, "loadBefore": bench.command("status"),
                     "transientHeapTolerance": 8192}
            try:
                matrix(fixture, bench, child)
            except Exception as error:
                child["error"] = str(error)
                failures.append("combined_" + name + "_matrix")
                print("FAIL " + failures[-1] + ": " + str(error), flush=True)
            child["loadAfter"] = bench.command("status")
            require(child["loadAfter"]["load"] == "combined", "Load stopped during matrix")
            report["matrices_" + name] = child
            if "error" not in child:
                record("combined_" + name + "_matrix", {"cases": len(child["tests"])})
        fixture.idle()
        bench.command("stop")
        bench.command("load none")
        time.sleep(1)
        after = bench.command("status")
        require(after["freeHeap"] >= baseline["freeHeap"] - 128, "Quiescent heap loss after load profiles")
        require(after["load"] == "none" and after["wifiMode"] == 0, "Load did not shut down")
        record("quiescent_heap_and_load_cleanup", {"before": baseline, "after": after})
        report["coverage_gaps"] = ["Wi-Fi AP plus active scans, not sustained external UDP payload traffic",
                                   "Busy intervals are synthetic bounded load, not every interrupt/flash workload",
                                   "busyUs includes preemption within each interval, not calibrated CPU utilization",
                                   "RP2040 sampled UART timing, not calibrated RS485 waveform qualification"]
        report["failed_cases"] = failures
        require(not failures, "Load qualification failed: " + ", ".join(failures))
    finally:
        bench.command("load none")


if __name__ == "__main__":
    sys.exit(run_harness(run_cases, "build/hil/s3-load-report.json"))
