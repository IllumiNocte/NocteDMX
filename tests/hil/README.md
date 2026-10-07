# Standalone hardware smoke tests

These tests use NocteDMX directly. They do not need uNode, Wi-Fi, a Raspberry
Pi host, Node-RED or a web API. GitHub CI compiles the test firmware, but the
physical tests run locally.
Firmware sketches live in `extras/hil`, as required by the Arduino library
layout. The host runner and its dependencies remain in `tests/hil`.

## Hardware

- ESP8266 with 4 MB flash and USB serial programming.
- RP2040/RP2350 with the uNode DMX tester firmware, tested with version 0.4.14.
- One RS485 transceiver per device, connected through a terminated DMX bus.
- ESP UART0 TX GPIO1 to DI, RX GPIO3 to RO, GPIO5 to tied DE and active-low /RE.
- RP2040 tester wiring follows its firmware's pin configuration.

Use only the synthetic tester responder. The test firmware changes start
address 42 to 43 and restores 42, then switches Identify on and off. It also
flashes the ESP and replaces the running firmware. Back up existing firmware
before the first run. The runner always puts the RP2040 in `idle` before
flashing: an enabled RS485 receiver can interfere with the ESP bootloader UART.
Cleanup returns the tester to the conformant profile and `idle`.

## Run

Install Arduino CLI, ESP8266 Arduino Core 3.1.2, and the Python dependencies:

```sh
python -m pip install -r tests/hil/requirements.txt
python tests/hil/run_smoke.py --esp-port COM12 --fixture-port COM3
```

Linux serial paths work as well. `--arduino-cli` accepts the full executable
path, `--direction-pin` changes tied DE and /RE, and `--tests output|input|rdm`
runs one group. `--report` selects the generated JSON report. Build artifacts
default to `build/hil`; dependencies are not installed by the runner.

The isolated Python bundled with Arduino may require a launcher that adds the
core's `tools/pyserial` directory to `sys.path`. For this installation, pass
`--esptool-script` and `--serial-module-path` for its bundled esptool child.
An ordinary Python installation with the requirements above needs neither.

## Coverage and limits

### ESP32-S3 tested scope

An experimental DMX-only path is prepared with
`--chip esp32s3 --tests dmx` (or `output`/`input`). The default S3 build uses
UART1 TX17/RX18 and direction pin 255 (disabled). It compiles for Arduino Core
3.3.12 with USB CDC on boot and flashes the **merged** image at offset 0.
Use esptool 4.x/5.x. `all` and `rdm` are rejected for S3 before opening ports.
This path passed on S3 UART1 with direct UART. See [the bring-up guide](../../docs/esp32s3.md)
for crossed 3.3-V UART wiring and the dedicated interactive bench sketch.
The ESP8266 tests/defaults remain unchanged.
`--esptool-executable` selects a bundled standalone programmer instead of a
Python module. The expanded `run_s3_bench.py` suite uses tester 0.4.15 or later
and covers 15 DMX/fault/lifecycle cases; `--no-flash` reuses the bench firmware.
See [the validation record](../../docs/esp32s3-validation.md) for measured results
and exclusions. UART2 now passes the same DMX/RDM/discovery matrices separately;
Wi-Fi stress, simultaneous ports and RS-485 electrical qualification
are not claimed by these direct-UART smoke tests.

The separate S3 RDM GET/SET runner builds/flashes the same interactive bench:

```sh
python tests/hil/run_s3_rdm.py --esp-port COM6 --fixture-port COM3
```

The S3 bench, RDM and discovery runners accept `--uart 2` to use UART2 on the same
TX17/RX18 wiring (`--uart 1` is the default). Firmware status must confirm the
requested UART, including with `--no-flash`; an older/mismatched bench is rejected.
UART1/UART2 builds and default reports are separated. To compare without repeated
flashing, run the DMX bench with `--uart 2`, then RDM/discovery with `--uart 2 --no-flash`.

It changes only the synthetic fixture, covers normal reads/writes, maximum
request size, malformed/timing responses and recovery, plus repeated commands
with DMX resumption/heap checks. Exact numerical timing limits are native-tested;
hardware boundary profiles use margin (120-us receive BREAK, 500-us excessive
BREAK, 200-us excessive MAB). Short-MAB hardware coverage is explicitly reported
as missing when `normalMabUs=0` still generates a legal MAB due to fixture software.
S3 discovery is implemented with native and direct-UART tests; its hardware matrix is
`run_s3_discovery.py` (same programmer/port arguments), requiring tester 0.4.17.
It covers 24 points including 20 repeated full scans; an early-delay fixture
profile that produces legal wire timing is explicitly recorded as a coverage
gap rather than a successful rejection. Responder and electrical qualification
remain pending.
Host-runner dry tests mock all serial/programming calls and verify image
selection and the S3 RDM guard without Python hardware dependencies:

```sh
python -m unittest discover -s tests/hil -p 'test_*.py' -v
```

### S3 synthetic CPU and Wi-Fi load

```sh
python tests/hil/run_s3_load.py --esp-port COM6 --fixture-port COM3 --uart 2
```

The runner flashes the interactive bench, then tests no load, CPU-only,
Wi-Fi-only and combined load. Each profile has three 30-second phases:
512-channel input, 512-channel output and interleaved RDM GET/SET/full discovery.
The existing DMX/RDM/discovery valid/fault/recovery matrices then run again at
combined load. JSON reports include counters, sampled timing, heap, actual
worker progress and completed WLAN scans. `--no-flash` requires the load-enabled
bench already running. Reports are separated by UART as for the other runners.

Bench command `load cpu` runs a priority-1 compute worker on each core with
7-ms work windows and 3-ms yields. `busyUs` includes preemption during those
windows: it is **not calibrated CPU utilization**. `load wifi` starts a temporary
AP (`NocteDMX-HIL`, password `nocte-hil-load`) plus repeated asynchronous active
scans; it uses no saved station credentials. `load combined` enables both.
`load none` disables computation/scans and turns Wi-Fi off; the two allocated
worker tasks stay parked for repeatable quiescent heap comparisons.

Cleanup restores load-none, input/verbose and the idle/default tester. This
qualifies synthetic compute and AP/scan activity only: sustained external UDP
payload traffic, flash/interrupt worst cases and electrical RS485 still need
separate tests. Timing snapshots and last-frame comparisons are not a lossless
waveform or proof that every transmitted frame was observed.

Live heap comparisons during WLAN scans allow at most 8192 bytes of temporary
variation; ordinary unloaded matrices still use 128 bytes. A separate warmed,
stopped-port/WLAN-off comparison at the end uses **128 bytes**, so transient
scan result allocation does not mask accumulated leaks. Protocol/timing checks
are unchanged. `--report` can preserve pre/post-hardening runs independently.

The S3 build guard `check_s3_iram.py --objdump TOOL --elf BENCH.ino.elf` checks
the placement and resolved code/data references of seven UART/GPIO timing
routines. It rejects the Xtensa size-optimized outlined atomic-boolean load in
flash; user callback function pointers are outside this limited static check.
CI runs the guard on its preserved UART2 bench ELF.
Arduino's outer GPIO dispatcher and cache-off interrupt masking are outside
the guard; our callback being IRAM-resident is not a cache-off receive guarantee.

### Independent S3 output timing windows

RP2040 tester **0.4.18** provides a separate PIO/DMA raw sampler on its existing
RX pin. `run_s3_pio_timing.py` accepts the same ports/programmer/`--uart` arguments
as the RDM runner; `--no-flash` reuses a verified bench. It stops the S3 before
the fixture's own TX-pin reference self-test (176-us BREAK, 16/8/7-us MAB), then
checks 30 seconds each of no load, CPU, AP/scans and combined-load 512-slot output.
The fixture stays idle: it does not decode payloads or act as a responder during
raw capture. This deliberately avoids its GPIO-edge timing ISR.

Each finite 64-KiB DMA window covers 131.072 ms at 4 MHz. Stalls, DMA timeouts,
wrong resolution/length, too few complete pairs and short MABs fail the runner.
Initial/final partial runs are excluded; every window's statistics/details are
recorded, along with the actual captured duration. There are decode/USB gaps
between windows. The reference and sampler share a clock, and no analog or
RS485 electrical behavior is measured. A pass is not continuous worst-case
conformance and does not replace the original payload/fault/RDM matrices.

### ESP8266 tested scope

- 24-slot DMX output: two identical changing fade channels and zero elsewhere.
- 512-slot DMX input: every received channel is echoed and compared, including 512.
- Constructed port and UART ownership, including an inactive second port destructor.
- Per-port controller UID, full-range discovery and Mute/Unmute.
- DEVICE_INFO with reported PDL and guard bytes beyond the real payload.
- GET and SET start address, read-back, restore, Identify and NACK result.
- Oversized SET rejected before accessing its short input buffer or sending.
- Correct-checksum wrong-transaction replies rejected, then clean recovery.
- Corrupt discovery checksum rejected, then clean recovery.
- Observed RDM request Break/MAB and post-response controller spacing.

The RP2040 analyzer's edge flags and USB status snapshots are not a lossless
logic-analyzer capture. At the output example's ~767 Hz rate it can occasionally
merge frame edges. Output timing therefore requires five intact snapshots and
records all attempted snapshots. RDM statistics include both normal DMX and RDM
traffic; `shortFrames` does not mean an invalid RDM frame. These are regression
smoke tests, not a complete DMX512-A/E1.20 certification or worst-case load test.
Sustained USB JSON serialization can also disrupt the tester's synthetic RDM
responder. RDM checks therefore use short snapshots with 3.5-second quiet
windows. A successful cycle proves the functional assertions, not zero loss
under console load; that fixture limitation needs separate stress coverage.

RDM responder mode on the ESP, split DE//RE wiring, multi-responder collisions,
all timing boundary profiles and prolonged external Wi-Fi traffic remain additional HIL
coverage. They are not claimed by a green run of this script.

### ESP8266 synthetic load and independent output timing

```sh
python tests/hil/run_esp8266_load.py --esp-port COM12 --fixture-port COM3 --phase rdm
python tests/hil/run_esp8266_load.py --esp-port COM12 --fixture-port COM3 --phase output
```

Two builds of one sketch use UART0 TX1/RX3, with direction pin 255 (direct UART).
The runner sets `NOCTE_HIL_RDM=0/1` and keeps separate output/RDM build folders.
Telemetry is embedded in 512 DMX slots; UART0 is not a debug console. Each boot
runs one finite sweep: 20 seconds each of no load, IRQ/foreground, AP/scans and
combined load, then stays at no load. The timer adds a maskable 100-us ISR every
millisecond, with 7-ms foreground work windows. This is not calibrated CPU
utilization or a worst-case NMI/cache-off workload. Wi-Fi uses a temporary AP
and active scans, with persistence disabled, not saved credentials or external
UDP traffic. The runner resets the ESP after the fixture is ready; old responder
absence must not contaminate cumulative failure counters.

RDM mode exercises DEVICE_INFO, Identify SET/read-back/restore, discovery/Mute
and maximum-PDL all-0xFF SET requests (unknown-PID NACK expected). Cumulative
failure/timeout counters, actual IRQ/scan progress and warmed quiescent heap
are checked. The synthetic responder is observed through quiet windows, not
continuous USB polling. The tester reports its most recently decoded packet,
including RDM, and serializing a 64-channel USB response can overrun the next
UART frame. Incomplete/non-DMX snapshots are preserved and retried for at most
10 seconds, not represented as valid telemetry or independently proven wire
packet loss. Complete 512-slot snapshots and the final slot guard are required;
the guard uses a quiet recovery interval before its single-channel query.

Output mode instead puts tester 0.4.18 into idle for independent PIO/DMA windows
(4 MHz, 64 KiB, 131.072 ms). Payload sampling and raw capture are separate.
Every raw window must have valid complete BREAK/MAB pairs; stalls, timeouts and
sub-88-us BREAK/sub-8-us MAB fail. Profile transitions are recorded separately;
each profile must have a stable window. These finite windows have observation
gaps and share the tester clock, so they do not certify electrical RS485 or
every DMX/RDM packet. Normal DMX scheduling remains byte-per-interrupt.

ESP8266 controller TX now protects its 200-us BREAK/20-us MAB plus the first
byte, then feeds/drains the bulk FIFO with interrupts enabled. The reserved
final byte's FIFO-to-shifter transition and RX/direction handoff are protected
separately, with a 120-us FIFO wait and 60-us tail guard. Request EOP is still
an estimate of the final 44-us wire slot, not a hardware TX_DONE timestamp.
FIFO feed/drain timeouts increment `rdmTransmitTimeoutCount()` and report a
receive error (or partial discovery), with RX rearmed before IRQs are restored.
The legacy frame-boundary pause/resume loops are not covered by that timeout.

The ESP8266 foreground `setTaskReceive()` handoff masks and acknowledges
TX-empty before publishing RECEIVE, with an interrupt lock protecting receive
state reset and direction release. Publishing RECEIVE while the level-triggered
empty-FIFO interrupt was still enabled could cause an interrupt storm: the ISR
would skip filling the FIFO and immediately retrigger before foreground could
disable it. Host tests guard publication order and model this unsafe
interleaving; they do not simulate the complete UART or prove all watchdog
causes absent.

On 2026-10-07 the patched backend passed a direct-UART integration test using
RP2040 tester 0.4.20: six 30-second phases covering Art-Net/sACN output, DMX input
forwarding through either protocol, valid RDM and two-worker HTTP load with
five independent zero-DMX PIO/DMA windows. A separate 300-second DEVICE_INFO
run passed another 135 cycles (149 total), with no restart and verified runtime
and persistent configuration restoration. The native core suite, 56 host cases
and six-routine ESP8266 IRAM guard also passed. The application used ordinary
external Wi-Fi UDP and HTTP traffic; this is finite integrated regression
evidence, not continuous worst-case timing, electrical RS485, malformed-RDM
qualification or a long-term soak.

`check_s3_iram.py --chip esp8266` also checks six ESP8266 helper/core functions
in the actual ELF. A forced-inline interrupt lock avoids a size-optimized core
destructor being placed in flash. The guard checks direct/literal-resolved
dependencies, not an unrestricted indirect call graph or cache-off guarantee.
