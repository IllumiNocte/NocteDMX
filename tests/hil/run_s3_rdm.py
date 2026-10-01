"""Direct-UART S3 RDM GET/SET and fault recovery; not RS485 certification."""
import argparse
import datetime
import json
from pathlib import Path
import sys
import time

from run_smoke import Fixture, build_and_flash, require
from run_s3_bench import Bench


def run_cases(fixture, bench, report):
    def record(name, result):
        report["tests"][name] = result
        print("PASS " + name, flush=True)

    def profile(name):
        fixture.command(cmd="rdm", action="profile", name=name)
        # GPIO/USB fixture timing is not calibrated to 1 us. Use margin for
        # wire-level assertions, native tests cover the exact numeric limits.
        if name == "short-break":
            fixture.command(cmd="rdm", action="fault", normalBreakUs=120)
        elif name == "long-break":
            fixture.command(cmd="rdm", action="fault", normalBreakUs=500)
        elif name == "long-mab":
            fixture.command(cmd="rdm", action="fault", normalMabUs=200)
        time.sleep(0.03)  # Keep USB fixture commands out of the RDM response window.

    def get(pid, capacity=231):
        return bench.command("get %x %d" % (pid, capacity), "rdm")

    def ack(reply, pdl=None):
        require(reply["status"] == 0 and reply["flags"] == 0,
                "Expected ACK: " + repr(reply))
        if pdl is not None:
            require(reply["pdl"] == pdl and reply["copied"] == pdl,
                    "Wrong parameter length: " + repr(reply))
        require(reply["guard"] == 165, "Response copy overwrote the guard byte")

    fixture.idle()
    fixture.command(cmd="rdm", action="defaults")
    fixture.command(cmd="rdm", action="configure", uid="7FF0:52444D01",
                    deviceLabel="NocteDMX S3 HIL", startAddress=42,
                    footprint=16, responseDelayUs=500)
    profile("conformant")
    fixture.command(cmd="mode", value="rdm")
    bench.command("rdm")
    invalid = bench.command("invalid", "invalid")
    require(invalid["statuses"] == [6] * 6, "Invalid arguments were not rejected: " + repr(invalid))
    record("invalid_arguments", invalid)
    details = get(0x0060)
    ack(details, 19)
    require(details["data"][10:12] == [0, 16] and details["data"][14:16] == [0, 42],
            "DEVICE_INFO footprint/address mismatch: " + repr(details))
    record("device_info", details)
    short = get(0x0060, 3)
    require(short["status"] == 0 and short["pdl"] == 19 and short["copied"] == 3
            and short["guard"] == 165 and short["data"] == details["data"][:3],
            "Bounded response copy failed: " + repr(short))
    record("bounded_copy", short)
    zero = get(0x0060, 0)
    require(zero["status"] == 0 and zero["copied"] == 0 and zero["guard"] == 165,
            "Zero-capacity response copy failed")
    record("zero_capacity", zero)
    for address in (43, 42):
        changed = bench.command("set f0 %04x" % address, "rdm")
        ack(changed)
        readback = get(0x00F0)
        ack(readback, 2)
        require(readback["data"] == [0, address], "SET address readback mismatch")
        record("set_address_" + str(address), {"set": changed, "get": readback})
    for identify in (1, 0):
        changed = bench.command("set 1000 %02x" % identify, "rdm")
        ack(changed)
        readback = get(0x1000)
        ack(readback, 1)
        require(readback["data"] == [identify], "SET identify readback mismatch")
        record("set_identify_" + str(identify), {"set": changed, "get": readback})
    nack = get(0xFFFE)
    require(nack["status"] == 1 and nack["pdl"] == 2 and nack["flags"] == 0,
            "Unknown PID did not produce a valid NACK: " + repr(nack))
    record("nack", nack)
    maximum = bench.command("set fffe " + "aa" * 231, "rdm")
    require(maximum["status"] == 1 and maximum["flags"] == 0 and maximum["pdl"] == 2,
            "Maximum 257-byte request did not return valid NACK: " + repr(maximum))
    record("maximum_request_fifo_refill", maximum)
    # Use 120 us with scheduling margin: below responder TX minimum, inside
    # controller RX range 88..352 us, so this wire profile must be accepted.
    for name in ("conformant", "earliest-valid", "latest-valid", "slow-frame-valid", "short-break"):
        profile(name)
        replies = [get(0x0060) for _ in range(3)]
        for reply in replies:
            ack(reply, 19)
        record("valid_" + name, replies)
    profile("short-mab")
    probe = get(0x0060)
    if 8 <= probe["timing"]["mabUs"] <= 88:
        ack(probe, 19)
        report["coverage_gaps"] = ["RP2040 short-mab=0 still produces legal wire MAB due to UART software overhead"]
        record("probe_short_mab_fixture_limit", probe)
    else:
        require(probe["status"] == 5 and probe["flags"] & 4096, "Invalid measured MAB was accepted")
        record("reject_short_mab", probe)
    for name in ("too-early", "too-late", "slow-frame-invalid", "no-break",
                 "long-break", "long-mab", "dropped",
                 "bad-checksum", "wrong-transaction", "wrong-destination", "wrong-source",
                 "wrong-command-class", "wrong-pid", "wrong-sub-device",
                 "invalid-response-type", "wrong-message-length", "wrong-pdl",
                 "wrong-sub-start", "truncated", "extra-byte"):
        profile(name)
        reply = get(0x0060)
        require(reply["status"] in (4, 5) and reply["copied"] == 0,
                "Malformed response was accepted (" + name + "): " + repr(reply))
        time.sleep(0.08)  # Drain deliberately late/slow fault traffic before recovery.
        profile("conformant")
        recovery = get(0x0060)
        ack(recovery, 19)
        record("reject_and_recover_" + name, {"fault": reply, "recovery": recovery})
    burst = bench.command("burst", "burst")
    require(burst["acked"] == 100 and burst["flags"] == 0,
            "Back-to-back foreground commands failed: " + repr(burst))
    require(burst["afterHeap"] >= burst["beforeHeap"] - 128, "Heap fell across foreground burst")
    record("100_back_to_back_foreground_commands", burst)
    baseline = bench.command("status")
    for _ in range(100):
        ack(get(0x0060), 19)
    after = bench.command("status")
    require(after["freeHeap"] >= baseline["freeHeap"] - 128, "Heap fell across 100 transactions")
    require(after["txTimeouts"] == 0, "UART transmit timeout")
    require(after["txFrames"] > baseline["txFrames"], "DMX output did not resume between commands")
    record("100_transactions_resume_dmx_stable_heap", {"before": baseline, "after": after})
    report["fixture_after"] = fixture.command(cmd="rdm", action="status")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-port", required=True)
    parser.add_argument("--fixture-port", required=True)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--esptool-executable")
    parser.add_argument("--no-flash", action="store_true")
    parser.add_argument("--report", default="build/hil/s3-rdm-report.json")
    args = parser.parse_args()
    if args.esp_port.lower() == args.fixture_port.lower():
        parser.error("S3 and fixture ports must differ")
    args.chip, args.direction_pin = "esp32s3", 255
    args.fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc"
    args.esptool_script = args.serial_module_path = None
    report = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "tests": {}, "ok": False}
    fixture = bench = None
    try:
        fixture = Fixture(args.fixture_port)
        report["fixture"] = fixture.command(cmd="ping")
        fixture.idle()
        if not args.no_flash:
            build_and_flash(args, fixture, "extras/hil/Esp32S3UartBench")
        bench = Bench(args.esp_port)
        run_cases(fixture, bench, report)
        report["ok"] = True
    except Exception as error:
        report["error"] = str(error)
        print("FAIL " + str(error), file=sys.stderr, flush=True)
    finally:
        # A failed ESP console must never prevent returning the fixture to idle.
        actions = []
        if bench:
            actions.append(lambda: bench.command("stop"))
        if fixture:
            actions.extend([fixture.idle, lambda: fixture.command(cmd="rdm", action="defaults")])
        if bench:
            actions.extend([lambda: bench.command("input"), lambda: bench.command("verbose")])
        for action in actions:
            try:
                action()
            except Exception as error:
                report.setdefault("cleanup_errors", []).append(str(error))
                report["ok"] = False
        if bench:
            bench.serial.close()
        if fixture:
            fixture.serial.close()
        path = Path(args.report)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("Report: " + str(path.resolve()), flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
