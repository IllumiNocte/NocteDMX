"""Standalone RP2040/ESP HIL smoke tests; no uNode firmware required."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import serial


class Fixture:
    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=0.3, write_timeout=2)
        self.serial.dtr = True
        self.serial.rts = True
        time.sleep(0.2)
        self.serial.reset_input_buffer()

    def command(self, **payload):
        self.serial.write(json.dumps(payload).encode() + b"\n")
        self.serial.flush()
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = self.serial.readline().strip()
            if not line.startswith(b"{"):
                continue
            reply = json.loads(line)
            if reply.get("event") == "ready":
                continue
            if not reply.get("ok", False):
                raise RuntimeError("Fixture command failed: " + repr(reply))
            return reply
        raise TimeoutError("No JSON response to " + repr(payload))

    def idle(self):
        return self.command(cmd="mode", value="idle")

    def frame(self, count=512):
        return self.command(cmd="get", target="frame", start=1, count=count)

    def wait_frame(self, predicate, timeout=12, count=512, interval=0.1):
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            time.sleep(interval)
            last = self.frame(count)
            if last.get("startCode") == 0 and predicate(last):
                return last
        raise AssertionError("Expected DMX result not observed: " + repr(last))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def build_and_flash(args, fixture, sketch):
    fixture.idle()  # Mandatory: a driving RS485 RO can block the USB bootloader.
    root = Path(__file__).resolve().parents[2]
    source = root / sketch
    build = root / "build" / "hil" / args.chip / source.name
    subprocess.run([
        args.arduino_cli, "compile", "--fqbn", args.fqbn,
        "--library", str(root), "--warnings", "all",
        "--build-path", str(build),
        "--build-property", "compiler.cpp.extra_flags=-DNOCTE_HIL_DIRECTION_PIN=" + str(args.direction_pin),
        str(source),
    ], check=True)
    executable = getattr(args, "esptool_executable", None)
    command = [executable] if executable else [sys.executable]
    if executable:
        pass  # Standalone vendor executable already bundles Python/serial.
    elif args.esptool_script and args.serial_module_path:
        # The isolated Python bundled with Arduino ignores PYTHONPATH.
        command += ["-c", "import sys, runpy; sys.path.insert(0, "
                    + repr(args.serial_module_path) + "); sys.argv=sys.argv[1:]; "
                    + "runpy.run_path(sys.argv[0], run_name='__main__')",
                    args.esptool_script]
    elif args.esptool_script:
        command.append(args.esptool_script)
    else:
        command += ["-m", "esptool"]
    suffix = ".ino.merged.bin" if args.chip == "esp32s3" else ".ino.bin"
    firmware = build / (source.name + suffix)
    if not firmware.is_file():
        raise FileNotFoundError("Expected flash image missing: " + str(firmware))
    command += ["--chip", args.chip, "--port", args.esp_port,
                "--baud", "460800", "write_flash", "0x0",
                str(firmware)]
    subprocess.run(command, check=True)


def test_output(args, fixture):
    build_and_flash(args, fixture, "examples/DmxOutputFade")
    fixture.command(cmd="mode", value="rx")
    fixture.command(cmd="clear", target="stats")
    frame = fixture.wait_frame(lambda f: f["slots"] == 24
        and f["values"][0] == f["values"][7]
        and all(v == 0 for i, v in enumerate(f["values"]) if i not in (0, 7)), count=24)
    first = frame["values"][0]
    fixture.wait_frame(lambda f: f["slots"] == 24 and f["values"][0] != first, count=24)
    samples = []
    valid = []
    for _ in range(20):
        stats = fixture.command(cmd="get", target="stats")
        samples.append(stats)
        # At ~767 Hz the RP2040 main loop can merge edge flags. These are
        # smoke measurements, not a lossless capture or a conformance claim.
        if (stats["lastSlots"] == 24 and stats["startCode"] == 0
                and stats["lastBreakUs"] >= 88 and stats["lastMabUs"] >= 8
                and 245000 <= stats["baudEstimate"] <= 255000):
            valid.append(stats)
        if len(valid) >= 5:
            break
        time.sleep(0.05)
    require(len(valid) >= 5, "Not enough intact timing snapshots: " + repr(samples))
    return {"frame": frame, "timing_samples": samples, "intact_samples": len(valid)}


def test_input(args, fixture):
    build_and_flash(args, fixture, "extras/hil/DmxInputSmoke")
    values = [(i * 17 + 3) & 255 for i in range(512)]
    fixture.command(cmd="set", target="frame", slots=512, values=values)
    fixture.command(cmd="tx", action="start")
    time.sleep(0.7)
    fixture.idle()
    fixture.command(cmd="mode", value="rx")
    frame = fixture.wait_frame(lambda f: f["slots"] == 512 and f["values"] == values)
    return {"frame": frame}


def test_rdm(args, fixture):
    build_and_flash(args, fixture, "extras/hil/ControllerSmoke")
    fixture.command(cmd="rdm", action="defaults")
    fixture.command(cmd="rdm", action="configure", uid="7FF0:52444D01",
                    deviceLabel="NocteDMX HIL", startAddress=42, footprint=16, responseDelayUs=500)
    fixture.command(cmd="rdm", action="profile", name="conformant")
    fixture.command(cmd="mode", value="rdm")

    def telemetry(f):
        v = f["values"]
        return f["slots"] == 512 and v[:3] == [0xA5, 0x4E, 1]

    def clean(f):
        if not telemetry(f):
            return False
        v = f["values"]
        return v[4:15] == [1, 2, 24, 1, 1, 19, 19, 1, 1, 1, 1] and v[15:21] == [43, 1, 1, 1, 1, 6]

    def wait_rdm(predicate):
        # The fixture services RX edges in its main loop. Sustained USB JSON
        # serialization can itself disrupt that synthetic responder. Observe
        # completed cycles with quiet windows instead of hammering its console.
        return fixture.wait_frame(predicate, timeout=45, count=64, interval=3.5)

    # Keep USB snapshots short while the tester must service RDM deadlines.
    frame = wait_rdm(clean)
    require(frame["values"][24:30] == [0x7F, 0xF0, 0x52, 0x44, 0x4D, 1], "Wrong discovered UID")
    require(fixture.frame()["values"][511] == 0x5A, "Full DMX frame was truncated")
    status = fixture.command(cmd="rdm", action="status")
    require(status["startAddress"] == 42 and not status["identifying"], "SET state was not restored")
    require(status["timing"]["requestBreakWithinE120"], "RDM request break out of bounds")
    require(status["timing"]["requestMabWithinE120"], "RDM request MAB out of bounds")
    require(status["timing"]["controllerPacketSpacingWithinE120"], "RDM controller spacing out of bounds")
    results = {"conformant": {"frame": frame, "responder": status}}

    # Correct-checksum answers with an unrelated transaction number must fail.
    sequence = frame["values"][3]
    fixture.command(cmd="rdm", action="fault", transactionDelta=1)
    bad = wait_rdm(lambda f: telemetry(f) and f["values"][3] != sequence
        and f["values"][5] == 2 and f["values"][7] == 0 and f["values"][8] == 0
        and (f["values"][21] & 8) != 0)
    results["wrong_transaction"] = bad

    sequence = bad["values"][3]
    fixture.command(cmd="rdm", action="profile", name="conformant")
    recovered = wait_rdm(lambda f: clean(f) and f["values"][3] != sequence)
    results["recovered_after_wrong_transaction"] = recovered

    sequence = recovered["values"][3]
    fixture.command(cmd="rdm", action="fault", corruptEvery=1)
    corrupted = wait_rdm(lambda f: telemetry(f) and f["values"][3] != sequence
        and f["values"][5] != 2 and f["values"][8] == 0)
    results["corrupt_checksum"] = corrupted
    sequence = corrupted["values"][3]
    fixture.command(cmd="rdm", action="profile", name="conformant")
    results["recovered_after_corruption"] = wait_rdm(
        lambda f: clean(f) and f["values"][3] != sequence)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-port", required=True)
    parser.add_argument("--fixture-port", required=True)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    programmer = parser.add_mutually_exclusive_group()
    programmer.add_argument("--esptool-script", help="Optional path to a bundled esptool.py")
    programmer.add_argument("--esptool-executable", help="Optional standalone esptool executable")
    parser.add_argument("--serial-module-path", help="pyserial directory for isolated Arduino Python")
    parser.add_argument("--chip", choices=("esp8266", "esp32s3"), default="esp8266")
    parser.add_argument("--fqbn", help="Override the chip-specific board definition")
    parser.add_argument("--direction-pin", type=int, help="Default: ESP8266=5, S3=255 (unused)")
    parser.add_argument("--tests", choices=("all", "dmx", "output", "input", "rdm"), default="all")
    parser.add_argument("--report", default="build/hil/report.json")
    args = parser.parse_args()
    if args.chip == "esp32s3" and args.tests in ("all", "rdm"):
        parser.error("ESP32-S3 RDM is not implemented; select --tests dmx, output or input")
    if args.fqbn is None:
        args.fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc" if args.chip == "esp32s3" else "esp8266:esp8266:generic"
    if args.direction_pin is None:
        args.direction_pin = 255 if args.chip == "esp32s3" else 5
    if not 0 <= args.direction_pin <= 255:
        parser.error("Direction pin must be 0..255 (255 disables direction control)")
    if os.path.normcase(args.esp_port) == os.path.normcase(args.fixture_port):
        parser.error("ESP and fixture ports must differ")
    report = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(), "chip": args.chip,
              "tests": {}, "ok": False}
    fixture = None
    try:
        fixture = Fixture(args.fixture_port)
        report["fixture"] = fixture.command(cmd="ping")
        for name, function in (("output", test_output), ("input", test_input), ("rdm", test_rdm)):
            if args.tests in ("all", name) or (args.tests == "dmx" and name in ("output", "input")):
                print("Running " + name, flush=True)
                report["tests"][name] = function(args, fixture)
                print("PASS " + name, flush=True)
        report["ok"] = True
    except Exception as error:
        report["error"] = str(error)
        print("FAIL " + str(error), file=sys.stderr)
    finally:
        if fixture:
            try:
                fixture.command(cmd="rdm", action="profile", name="conformant")
                fixture.idle()
            except Exception as error:
                report["cleanup_error"] = str(error)
                report["ok"] = False
            fixture.serial.close()
        path = Path(args.report)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("Report: " + str(path.resolve()), flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
