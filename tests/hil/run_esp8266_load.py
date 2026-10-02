"""ESP8266 timed IRQ/foreground/WLAN profiles; telemetry travels as DMX.

Two separately flashed modes: passive PIO output timing or RDM GET/SET/discovery.
No UART0 console, extra wires, external UDP or calibrated conformance claim.
"""
import argparse
import datetime
import json
from pathlib import Path
import sys
import time
import serial
from run_smoke import Fixture, build_and_flash, require
from run_s3_load import delta


def telemetry(frame, rdm):
    values = frame.get("values", [])
    require(frame.get("slots") == 512 and len(values) >= 64
            and values[:3] == [0xA5, 0xD6, 1], "Wrong ESP8266 load firmware/telemetry")
    require(values[4] == int(rdm) and values[5] == 1 and values[3] in range(4),
            "Inactive/wrong ESP8266 load mode")
    result = {"profile": values[3], "cycle": int.from_bytes(bytes(values[6:8]), "big"),
              "failures": values[8], "wifiMode": values[9], "cpuMHz": values[11]}
    for offset, name in ((12, "freeHeap"), (16, "txTimeouts"), (20, "irqCount"),
                         (24, "scansCompleted"), (28, "cpuWindows"), (32, "goodGets"),
                         (36, "badGets"), (40, "scanAttempts"), (44, "badScans"),
                         (48, "badLongRequests"), (52, "firstSlotDelayUs"),
                         (56, "maximumSlotIntervalUs"), (60, "scanFailures")):
        result[name] = int.from_bytes(bytes(values[offset:offset + 4]), "big")
    require(result["failures"] == 0 and result["txTimeouts"] == 0
            and result["badGets"] == 0 and result["badScans"] == 0
            and result["badLongRequests"] == 0 and result["scanFailures"] == 0,
            "ESP8266 load failures: " + repr(result))
    require(result["wifiMode"] == (3 if result["profile"] & 2 else 0), "Wi-Fi profile not active")
    return result


def verify_raw(reply):
    require(reply.get("method") == "pio_dma_raw" and reply.get("valid") is True
            and not reply["stalled"] and not reply["timedOut"], "Invalid PIO capture")
    require(reply["words"] == 16384 and abs(reply["sampleUs"] - .25) < .001,
            "Unexpected capture rate/size")
    require(reply["breakUs"]["n"] >= 2 and reply["breakUs"]["n"] == reply["mabUs"]["n"],
            "Too few complete output pulses")
    require(reply["breakUs"]["min"] >= 88 and reply["mabUs"]["min"] >= 8
            and reply["shortMabs"] == 0, "Output timing below DMX limits: " + repr(reply))


def validate_progress(samples, rdm):
    for profile in range(4):
        group = [s for s in samples if s["profile"] == profile]
        require(len(group) >= 3, "Missing stable samples for profile %d" % profile)
        before, after = group[0], group[-1]
        require(after["cycle"] != before["cycle"], "Frozen telemetry")
        if profile & 1:
            require(delta(after["irqCount"], before["irqCount"]) >= 1000
                    and delta(after["cpuWindows"], before["cpuWindows"]) > 0,
                    "Interrupt/foreground load did not progress")
        if profile & 2:
            require(delta(after["scansCompleted"], before["scansCompleted"]) >= 1,
                    "No completed Wi-Fi scans in profile")
        if rdm:
            require(delta(after["goodGets"], before["goodGets"]) >= 3,
                    "Too few successful DEVICE_INFO transactions")
            require(delta(after["scanAttempts"], before["scanAttempts"]) >= 1,
                    "No complete discovery scan in profile")


def reset_esp(port):
    # Preconfigure modem lines before opening: opening with pyserial defaults
    # can assert GPIO0/reset and accidentally enter the bootloader.
    connection = serial.Serial(None, baudrate=74880, timeout=.2)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    try:
        connection.rts = True
        time.sleep(.1)
        connection.rts = False
    finally:
        connection.close()


def run_cases(fixture, report, rdm, esp_port):
    fixture.idle()
    if rdm:
        fixture.command(cmd="rdm", action="defaults")
        fixture.command(cmd="rdm", action="configure", uid="7FF0:52444D01", responseDelayUs=500)
        fixture.command(cmd="mode", value="rdm")
    else:
        fixture.command(cmd="mode", value="rx")
    # Restart only after the responder is ready. Cumulative counters must not
    # include transactions during flashing/setup of the synthetic fixture.
    reset_esp(esp_port)
    time.sleep(2)
    samples, captures = [], []
    report.update(samples=samples, captures=captures)
    deadline = time.monotonic() + 105
    started = time.monotonic()
    last_valid = started
    while time.monotonic() < deadline:
        time.sleep(3.5 if rdm else .1) # Avoid hammering the synthetic responder.
        snapshot = fixture.frame(count=64)
        # Ignore initial boot data / an old complete frame until the new image
        # has published telemetry, but never reinterpret failure flags as stale.
        if (snapshot.get("values", [])[:3] != [0xA5, 0xD6, 1]
                or snapshot.get("startCode") != 0 or snapshot.get("slots") != 512):
            report["last_unrecognized_frame"] = snapshot
            report.setdefault("non_telemetry_snapshots", []).append(snapshot)
            # RDM requests legitimately replace the last captured DMX frame.
            require(time.monotonic() - last_valid < 10, "Load firmware did not publish telemetry")
            continue
        report["last_frame"] = snapshot
        sample = telemetry(snapshot, rdm)
        last_valid = time.monotonic()
        sample["hostElapsedSeconds"] = time.monotonic() - started
        samples.append(sample)
        if not rdm:
            fixture.idle()
            capture = fixture.command(cmd="timing", action="capture")
            captures.append({"beforeProfile": sample["profile"], "capture": capture})
            verify_raw(capture)
            fixture.command(cmd="mode", value="rx")
            after = telemetry(fixture.wait_frame(
                lambda f: f.get("slots") == 512 and f.get("values", [])[:3] == [0xA5, 0xD6, 1],
                timeout=3, count=64), False)
            captures[-1]["afterProfile"] = after["profile"]
            captures[-1]["stableProfile"] = after["profile"] == sample["profile"]
        seen = {s["profile"] for s in samples}
        if len(seen) == 4 and sample["profile"] == 0 and time.monotonic() - started > 65:
            break # Full cycle, back in comparable warmed no-load state.
    validate_progress(samples, rdm)
    if not rdm:
        require({c["beforeProfile"] for c in captures if c["stableProfile"]} == set(range(4)),
                "Missing stable PIO window in a load profile")
    # Serializing 64 channel values can overrun the tester's next RX frame.
    # Allow several quiet full frames, then check only the final slot.
    time.sleep(.3)
    guard = fixture.command(cmd="get", target="frame", start=512, count=1)
    report["final_slot_guard"] = guard
    require(guard.get("slots") == 512 and guard.get("startCode") == 0
            and guard["values"] == [0x5A], "Full 512-slot telemetry truncated")
    quiescent = [s for s in samples if s["profile"] == 0]
    require(quiescent[-1]["freeHeap"] >= quiescent[0]["freeHeap"] - 1024,
            "Warmed no-load heap did not recover")
    report["quiescent_heap"] = [quiescent[0]["freeHeap"], quiescent[-1]["freeHeap"]]
    report["coverage_gaps"] = [
        "Single-core foreground work and a 100-us/1-ms maskable timer ISR, not calibrated CPU utilization",
        "AP plus active Wi-Fi scans, not sustained external UDP",
        "PIO windows have gaps and cannot run concurrently with RDM responder mode",
        "No physical RS485/DE/RE/contended bus or NMI/cache-off qualification",
        "Normal DMX byte-per-IRQ scheduler is unchanged; precise RDM first-slot timestamps remain estimates"]
    print("PASS all four ESP8266 load profiles (%s)" % ("RDM" if rdm else "PIO output"), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-port", required=True)
    parser.add_argument("--fixture-port", required=True)
    parser.add_argument("--phase", choices=("rdm", "output"), required=True)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--esptool-executable")
    parser.add_argument("--no-flash", action="store_true")
    parser.add_argument("--report")
    args = parser.parse_args()
    args.chip, args.direction_pin = "esp8266", 255
    args.fqbn = "esp8266:esp8266:generic"
    args.esptool_script = args.serial_module_path = None
    args.extra_build_flags = "-DNOCTE_HIL_RDM=" + str(int(args.phase == "rdm"))
    args.build_variant = args.phase
    if args.esp_port.lower() == args.fixture_port.lower(): parser.error("Ports must differ")
    report = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(), "ok": False,
              "phase": args.phase}
    fixture = None
    try:
        fixture = Fixture(args.fixture_port)
        report["fixture"] = fixture.command(cmd="ping")
        if not args.no_flash:
            build_and_flash(args, fixture, "extras/hil/Esp8266LoadBench")
        run_cases(fixture, report, args.phase == "rdm", args.esp_port)
        report["ok"] = True
    except Exception as error:
        report["error"] = str(error)
        print("FAIL " + str(error), file=sys.stderr, flush=True)
    finally:
        if fixture:
            try:
                fixture.idle()
                fixture.command(cmd="rdm", action="defaults")
            except Exception as error:
                report["cleanup_error"] = str(error)
                report["ok"] = False
            fixture.serial.close()
        path = Path(args.report or "build/hil/esp8266-load-%s.json" % args.phase)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("Report: " + str(path.resolve()), flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__": sys.exit(main())
