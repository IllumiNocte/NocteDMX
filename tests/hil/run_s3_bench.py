"""ESP32-S3 direct-UART DMX bring-up/fault tests. Flashes S3, not the RP2040."""
import argparse
import datetime
import json
from pathlib import Path
import sys
import time

import serial
from run_smoke import Fixture, build_and_flash, require


def verify_uart(bench, uart):
    status = bench.command("status")
    require(status.get("uart") == uart,
            "Bench UART mismatch: expected UART%d, got %r; reflash with --uart %d"
            % (uart, status.get("uart"), uart))
    return status


def uart_report_path(default, uart):
    path = Path(default)
    return str(path.with_name(path.stem + "-uart2" + path.suffix)) if uart == 2 else str(path)


def heap_tolerance(report):
    # Unloaded matrices keep their tight 128-byte check. WLAN scans allocate
    # temporary results asynchronously; the load harness separately checks
    # quiescent heap after shutting WLAN off against the original baseline.
    value = report.get("transientHeapTolerance", 128)
    require(isinstance(value, int) and 128 <= value <= 8192, "Invalid transient heap tolerance")
    return value


class Bench:
    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=0.2, write_timeout=3)
        self.serial.dtr = True
        self.serial.rts = False
        time.sleep(0.5)
        self.command("quiet")

    def command(self, text, kind="status", timeout=5):
        self.serial.reset_input_buffer()
        payload = text.encode("ascii") + b"\n"
        # USB CDC/JTAG RX buffering can be smaller than a maximum RDM SET
        # command. Pace long lines so the foreground parser can drain it.
        for start in range(0, len(payload), 48):
            self.serial.write(payload[start:start + 48])
            if len(payload) > 48:
                time.sleep(0.005)
        self.serial.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().strip()
            if not line.startswith(b"{"):
                continue
            reply = json.loads(line)
            if reply.get("type") == kind:
                if kind == "status":
                    require(reply["error"] == 0, "S3 setup error: " + repr(reply))
                return reply
        raise TimeoutError("S3 did not answer " + text)

    def frame(self):
        return self.command("frame", "frame")

    def wait_frame(self, values, timeout=5):
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            last = self.frame()
            if last.get("slots") == len(values) and last.get("values") == values:
                return last
            time.sleep(0.05)
        raise AssertionError("S3 frame mismatch: " + repr(last))


def run_cases(fixture, bench, report, skip_start_code=False):
    def send(values, *, start_code=0, break_us=176, mab_us=16, gap_us=0, fps=40):
        fixture.idle()
        fixture.command(cmd="set", target="timing", breakUs=break_us, mabUs=mab_us,
                        interSlotUs=gap_us, mbbUs=0, baud=250000, fps=fps)
        fixture.command(cmd="set", target="frame", slots=len(values),
                        values=values, startCode=start_code)
        fixture.command(cmd="tx", action="start")

    def record(name, result):
        report["tests"][name] = result
        print("PASS " + name, flush=True)

    bench.command("input")
    for slots in (1, 24, 511, 512):
        values = [(i * 17 + slots) & 255 for i in range(slots)]
        send(values)
        record("input_" + str(slots), bench.wait_frame(values))

    values = [(i * 13 + 9) & 255 for i in range(24)]
    send(values, gap_us=1000, fps=10)
    record("input_1ms_inter_slot", bench.wait_frame(values))
    long_break_values = [(i * 13 + 19) & 255 for i in range(24)]
    send(long_break_values, break_us=1000)
    record("input_long_break", bench.wait_frame(long_break_values))
    fixture.idle()
    before = bench.frame()
    invalid = [222] * 24
    if skip_start_code:
        report["tests"]["unknown_start_code_rejected"] = {
            "skipped": True, "reason": "Unmodified tester 0.4.14 overwrites startCode with 0 before TX"}
        print("SKIP unknown_start_code_rejected (tester limitation)", flush=True)
    else:
        send(invalid, start_code=0xCC)
        time.sleep(0.5)
        sent = fixture.frame(count=1)
        require(sent["startCode"] == 0xCC,
                "Tester did not preserve requested startCode: " + repr(sent))
        after = bench.frame()
        report["unknownStartCodeObservation"] = {"before": before, "after": after,
                                                  "status": bench.command("status")}
        require(after == before, "Unknown start code overwrote the last good frame: " + repr(after))
        first = bench.command("status")
        time.sleep(0.3)
        second = bench.command("status")
        require(first["rxFrames"] == second["rxFrames"], "Unknown start code was published")
        record("unknown_start_code_rejected", second)

    send(invalid, break_us=64)
    time.sleep(0.5)
    require(bench.frame() == before, "Short BREAK overwrote the last good frame")
    record("short_break_rejected", bench.command("status"))
    send([])
    time.sleep(0.3)
    require(bench.frame() == before, "Empty packet overwrote the last good frame")
    record("empty_packet_rejected", bench.command("status"))

    fixture.idle()
    fixture.command(cmd="noise", durationMs=200, minPulseUs=2, maxPulseUs=70)
    clean = [(i * 17 + 3) & 255 for i in range(512)]
    send(clean)
    record("recovery_after_noise", bench.wait_frame(clean))

    # Moderate USB/console activity while receiving full 512-channel packets.
    started = time.monotonic()
    initial = bench.command("status")
    snapshots = []
    while time.monotonic() - started < 5:
        bench.wait_frame(clean, timeout=1)
        snapshots.append(bench.command("status"))
        time.sleep(0.1)
    require(snapshots[-1]["rxFrames"] - initial["rxFrames"] >= 100,
            "Not enough full frames received during console activity")
    require(snapshots[-1]["rxErrors"] == initial["rxErrors"],
            "New UART errors during clean input with console activity")
    record("full_input_console_activity", {"durationSeconds": time.monotonic() - started,
           "first": initial, "last": snapshots[-1], "snapshots": len(snapshots)})

    fixture.idle()
    for slots, command in ((24, "output24"), (512, "output")):
        bench.command(command)
        fixture.command(cmd="mode", value="rx")
        values = clean[:slots]
        frame = fixture.wait_frame(lambda f: f["slots"] == slots and f["values"] == values,
                                   count=slots)
        time.sleep(1.1)
        stats = fixture.command(cmd="get", target="stats")
        require(35 <= stats["fps"] <= 45, "Unexpected output refresh: " + repr(stats))
        require(stats["lastBreakUs"] >= 88 and stats["lastMabUs"] >= 8,
                "Output timing sample below minimum")
        record("output_" + str(slots), {"frame": frame, "fixture": stats,
                                      "s3": bench.command("status")})
        bench.command("stop")
        fixture.idle()

    # Warm up task allocation/freeing, then check stopped-state heap for leaks.
    def cycle():
        require(bench.command("output")["active"], "Output failed to start")
        stopped = bench.command("stop")
        require(not stopped["active"], "Output failed to stop")
        require(bench.command("input")["active"], "Input failed to start")
        require(not bench.command("stop")["active"], "Input failed to stop")
        time.sleep(0.01)  # Let the idle task reclaim a deleted output task.

    for _ in range(5):
        cycle()
    baseline = bench.command("status")
    for _ in range(100):
        cycle()
    time.sleep(0.2)
    final = bench.command("status")
    require(final["freeHeap"] >= baseline["freeHeap"] - heap_tolerance(report),
            "Stopped-state heap fell across lifecycle cycles")
    require(final["txTimeouts"] == 0, "TX timed out during lifecycle cycles")
    record("lifecycle_100_cycles", {"baseline": baseline, "final": final})
    bench.command("input")
    send(clean)
    record("recovery_after_lifecycle", bench.wait_frame(clean))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-port", required=True)
    parser.add_argument("--fixture-port", required=True)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--esptool-executable")
    parser.add_argument("--uart", type=int, choices=(1, 2), default=1,
                        help="S3 UART peripheral; TX17/RX18 wiring stays unchanged")
    parser.add_argument("--report")
    parser.add_argument("--no-flash", action="store_true", help="Use an already flashed bench sketch")
    parser.add_argument("--skip-start-code", action="store_true", help="Explicit skip for unmodified tester 0.4.14")
    args = parser.parse_args()
    args.report = args.report or uart_report_path("build/hil/s3-bench-report.json", args.uart)
    if args.esp_port.lower() == args.fixture_port.lower():
        parser.error("S3 and fixture ports must differ")
    args.chip, args.direction_pin = "esp32s3", 255
    args.fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc"
    args.esptool_script = args.serial_module_path = None
    report = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "chip": args.chip, "uart": args.uart, "tests": {}, "ok": False}
    fixture = bench = None
    try:
        fixture = Fixture(args.fixture_port)
        report["fixture"] = fixture.command(cmd="ping")
        fixture.idle()
        if not args.no_flash:
            build_and_flash(args, fixture, "extras/hil/Esp32S3UartBench")
        bench = Bench(args.esp_port)
        report["bench"] = verify_uart(bench, args.uart)
        run_cases(fixture, bench, report, args.skip_start_code)
        report["ok"] = True
    except Exception as error:
        report["error"] = str(error)
        print("FAIL " + str(error), file=sys.stderr, flush=True)
    finally:
        try:
            if fixture:
                fixture.idle()
                fixture.command(cmd="set", target="timing", breakUs=176, mabUs=16,
                                interSlotUs=0, mbbUs=0, baud=250000, fps=40)
                fixture.command(cmd="set", target="frame", slots=512, startCode=0,
                                values=[(i * 17 + 3) & 255 for i in range(512)])
            if bench:
                bench.command("input")
                bench.command("verbose")
        except Exception as error:
            report["cleanup_error"] = str(error)
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
