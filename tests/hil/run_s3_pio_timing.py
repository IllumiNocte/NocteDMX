"""Independent finite PIO/DMA timing windows; RP2040 tester 0.4.18 required.

No payload decoding/responder operation during raw capture. Windows have gaps;
this is not continuous electrical conformance or calibrated clock validation.
"""
import sys
import time
from run_smoke import require
from run_s3_rdm import main as run_harness
from run_s3_load import delta, verify_load


def verify_capture(reply):
    require(reply.get("method") == "pio_dma_raw", "PIO/DMA tester firmware required")
    require(reply.get("valid") is True and not reply.get("stalled")
            and not reply.get("timedOut"), "Invalid PIO/DMA capture: " + repr(reply))
    require(reply["words"] == 16384 and abs(reply["sampleUs"] - 0.25) < 0.001,
            "Unexpected raw capture rate/length")
    require(reply["breakUs"]["n"] >= 3 and reply["mabUs"]["n"] == reply["breakUs"]["n"],
            "Not enough complete BREAK/MAB pairs: " + repr(reply))
    require(reply["breakUs"]["min"] >= 88 and reply["mabUs"]["min"] >= 8
            and reply["shortMabs"] == 0, "PIO output timing below DMX limits: " + repr(reply))


def run_cases(fixture, bench, report):
    fixture.idle()
    bench.command("load none")
    bench.command("stop")
    selftest = fixture.command(cmd="timing", action="selftest")
    report["tests"]["pio_reference_selftest"] = selftest
    require(selftest.get("method") == "pio_dma_raw" and selftest.get("passed") is True,
            "PIO hardware reference self-test failed")
    require([case["expectedMabUs"] for case in selftest["cases"]] == [16, 8, 7],
            "Missing reference/short-MAB test")
    print("PASS pio_reference_selftest (176-us BREAK, 16/8/7-us MAB)", flush=True)
    failures = []
    try:
        # Warm/park the same radio and worker resources before comparing heap.
        bench.command("load combined")
        time.sleep(3)
        bench.command("load none")
        time.sleep(1)
        baseline = bench.command("status")
        for profile in ("none", "cpu", "wifi", "combined"):
            bench.command("load " + profile)
            bench.command("output")
            time.sleep(2)
            before = bench.command("status")
            started = time.monotonic()
            captures, errors = [], []
            while time.monotonic() - started < 30:
                capture = fixture.command(cmd="timing", action="capture")
                captures.append(capture)
                try:
                    verify_capture(capture)
                except AssertionError as error:
                    errors.append(str(error))
            after = bench.command("status")
            result = verify_load(before, after, profile, time.monotonic() - started)
            result.update(captures=captures, errors=errors,
                          capturedSeconds=sum(c.get("durationUs", 0) for c in captures) / 1e6,
                          completePairs=sum(c["breakUs"]["n"] for c in captures),
                          minimumBreakUs=min(c["breakUs"]["min"] for c in captures),
                          minimumMabUs=min(c["mabUs"]["min"] for c in captures),
                          maximumMabUs=max(c["mabUs"]["max"] for c in captures))
            require(delta(after["txFrames"], before["txFrames"]) >= result["durationSeconds"] * 36,
                    "S3 output rate fell under raw timing capture")
            report["tests"][profile + "_pio_output"] = result
            if errors:
                failures.append(profile)
            print(("FAIL " if errors else "PASS ") + profile + "_pio_output: %d pairs, MAB %.2f..%.2f us"
                  % (result["completePairs"], result["minimumMabUs"], result["maximumMabUs"]), flush=True)
            bench.command("stop")
        bench.command("load none")
        time.sleep(1)
        after = bench.command("status")
        report["tests"]["quiescent_heap"] = {"before": baseline, "after": after}
        require(after["freeHeap"] >= baseline["freeHeap"] - 128 and after["wifiMode"] == 0,
                "PIO test load/heap cleanup failed")
        report["coverage_gaps"] = [
            "Finite 131-ms windows with decode/USB gaps, not continuous capture",
            "Self-test source and sampler share RP2040 clock, not absolute calibration",
            "No analog waveform, RS485 voltage/contention or payload validation",
            "RP2040 raw capture is idle-only, not simultaneous RDM responder operation"]
        report["failed_profiles"] = failures
        require(not failures, "PIO timing failed: " + ", ".join(failures))
    finally:
        bench.command("load none")


if __name__ == "__main__":
    sys.exit(run_harness(run_cases, "build/hil/s3-pio-timing-report.json"))
