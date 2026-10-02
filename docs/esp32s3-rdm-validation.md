# ESP32-S3 UART1/2 RDM controller validation

Date: **2026-10-01**. Direct 3.3-V UART, not RS-485 certification.
See [the setup/DMX record](esp32s3-validation.md) for board, core and wiring.
RP2040 tester firmware 0.4.15 was not changed for this milestone.

## Result

`tests/hil/run_s3_rdm.py` records **37 passing functional checks plus one
fixture-limit probe** (38 report entries). The 15 original DMX/fault/lifecycle
checks also pass after these changes. Native core tests and ten hardware-free
host-runner tests pass; the ESP8266 ControllerSmoke still compiles on Core 3.1.2.

Passing RDM scope:

- DEVICE_INFO: all 19 bytes, footprint 16, address 42, and bounded/zero-capacity copies.
- SET address 42->43->42 and Identify on/off, each with GET readback.
- Valid unknown-PID NACK; maximum 257-byte SET request exercises FIFO refill.
- Six invalid-argument guards: broadcast, null buffer/data and excessive PDL.
- Conformant, earliest/latest fixture delay, slow valid frame and 120-us receive BREAK.
- Twenty malformed/missing/timing profiles, each followed by successful conformant recovery:
  early/late response, excessive inter-slot delay, omitted/excessive BREAK,
  excessive MAB, dropped response, bad checksum, wrong transaction, destination,
  source, command class, PID, sub-device, response type, Message Length, PDL,
  Sub-START Code, truncated and extra-byte responses.
- 100 back-to-back DEVICE_INFO calls inside the S3 foreground task, then 100
  host-paced calls with successful DMX output resumption.

Free heap for the foreground burst: **338212 -> 338212 bytes**.
Host-paced sequence: **338212 -> 338212 bytes**, with 100 additional DMX frames
and zero TX timeouts. This is a repeatability/leak check, not every error-path proof.
The worker is allocated once at start and pauses at completed DMX frame boundaries.

## Timing and fixture limits

The timing implementation/native boundary tests use ANSI E1.20-2025 Tables
3-1/3-2: controller receive BREAK 88..352 us, MAB 8..88 us, request EOP to response
SOP 176..2800 us, missing-response wait at least 3000 us, byte interval at most
2144 us including the 44-us slot. Total packet duration and maximum-size
responses with the latest SOP are also native-tested, including timer wraparound.
A still-low BREAK that starts before the SOP deadline keeps capture alive across
the 3-ms missing-response deadline until its trailing edge can be qualified.

Hardware sampling uses GPIO/UART ISR timestamps, not a calibrated capture.
The RP2040's configured timing and actual wire timing differ due to UART/software
overhead. One-microsecond fault margins are therefore not reliable here:

- Receive short-BREAK check uses 120 us (below responder TX minimum 176 us,
  but within the required controller RX range).
- Excessive BREAK/MAB checks use 500/200 us rather than nominal 353/89 us.
- The `short-mab` profile configured to zero still generated about 22 us on the
  wire and was correctly accepted. **Sub-8-us MAB hardware rejection is not
  covered**; it has native tests only. No tester change/false conformance pass.

Typical RP2040 request snapshots: BREAK 176..177 us, MAB 18 us; subsequent controller
packet spacing exceeded 176 us. S3 byte timestamps describe interrupt service,
not independently measured second-stop-bit edges. These samples cannot certify
worst-case timings, UART interrupt latency or DE release.

The S3 conservatively rejects extra bytes within its response quiet window;
ESP8266 retains its existing policy of discarding post-EOP activity. An overlong
response never writes beyond the S3's fixed 257-byte buffer or publishes a prefix.

## Discovery milestone (2026-10-02)

Discovery/Mute/Unmute and the portable bounded scan are implemented. Native
tests cover 0..7 preamble bytes, invalid masks/checksum, GPIO-only activity,
timing bounds, fixed-buffer overflow, four-device collisions, malformed leaves,
table capacity, transaction limits and unresolved mute ACKs. Thirteen host-runner
dry tests pass; the ESP8266 ControllerSmoke still builds.
The existing GET/SET matrix was rerun on the flashed discovery build: all 37
functional checks and the short-MAB fixture-limit probe pass, including the
100-command foreground burst and 100 host-paced reads with DMX resumption.
Heap stayed at 337644 bytes across both sequences; zero transmit timeouts.
All 15 original DMX/fault/lifecycle checks also pass, including 100 lifecycle
cycles.

The dedicated discovery matrix passes with tester **0.4.17**: 24 report entries,
full/inclusive/excluded ranges, unicast/all/manufacturer MUTE/UNMUTE, normal GET
while muted, corrupt/truncated/extra-byte/late reply rejection and recovery,
dropped responses, scan transaction budgets, full scans and 20 repeated scans
with DMX resumption and heap checks (337644 -> 337644 bytes, 80 additional DMX
frames, no TX timeout). Repeated complete runs passed. Depending on
actual SOP timing the early-response case is a rejection or a fixture-limit
probe: configured delay zero sometimes still produces a legal >=176-us delay.
Native tests cover 175/176-us acceptance boundaries; ISR estimates are not
calibrated physical proof. Full electrical/timing qualification is still pending.

Two tester bugs were found and fixed before claiming passing HIL:

- The GPIO BREAK detector reused stale falling-edge timestamps when a short
  0xFF start bit was missed. Initially 7/20 discovery requests and 1/15 long-FF
  SET requests were complete; paired-edge guarding changed these to 20/20 and
  15/15, with zero length errors. The early failed run was not counted as a pass.
- The too-early DUB profile prestarted a normal UART BREAK and never released
  it, blocking TX completion. Only DISC_UNIQUE_BRANCH is excluded from normal
  BREAK preparation; MUTE/UNMUTE still use normal response framing.

## UART2 comparison (2026-10-02)

The same bench was built/flashed with `--uart 2`, on the unchanged GPIO17/18
wiring and tester 0.4.17. Firmware status confirms UART2 before each matrix.
All **37 GET/SET functional checks plus the short-MAB fixture-limit probe**
pass (38 entries). All **24 discovery checks** pass, including measured early/
late response rejection in this run, error recovery, MUTE/UNMUTE broadcast
scope, budgets and repeated full scans. No controller-backend changes were
needed; UART1 default compilation still passes.

- 100 consecutive foreground GETs and 100 host-paced GETs: heap **337644 ->
  337644 bytes**, no TX timeout; 100 additional DMX frames in the host-paced run.
- 20 full scans: heap **337644 -> 337644 bytes**, 80 additional DMX frames,
  each scan completed in four transactions with one confirmed UID.
- All 15 DMX/fault/lifecycle cases also pass separately on UART2.
- 19 hardware-free host-runner tests and the native core suite pass.

Generated reports: `build/hil/s3-rdm-report-uart2.json` and
`build/hil/s3-discovery-report-uart2.json`. UART1 reports are preserved. Cleanup
leaves the tester idle/default and the S3 on UART2 input/verbose. The short-MAB
fixture gap and RS485/load/electrical exclusions above still apply; a successful
separate UART2 run does not qualify simultaneous multi-port operation.

## Remaining work

### CPU-load timing regression before hardening (2026-10-02)

The new `run_s3_load.py --uart 2` bench uses priority-1 compute workers on both
cores (7-ms windows / 3-ms yields) and separate AP/active-scan WLAN profiles.
**The original S3 controller failed this load qualification.** In the initial CPU
profile, clean 512-slot DMX input/output passed, but a DEVICE_INFO response was
rejected with `flags: 4` (`kRdmResponseTooEarly`). S3 reported EOP-to-SOP 160 us;
the RP2040 reported response start delay 557 us. The response bytes were complete
and correlated, with measured BREAK/MAB 173/29 us.

At that milestone the transport polled UART TX-idle from the foreground task and took
`requestEnd` only after observing idle. A preemption between physical EOP and
that observation can move the reference timestamp later and reduce the measured
reply spacing; it can also delay receiver arming/direction release. The paired
measurements support this as the leading cause, not a calibrated proof of the
exact waveform. Thresholds were not widened to hide the failure, and that initial
load-test milestone did not include a backend fix.

Earlier aborted reports are kept locally: `s3-load-report-uart2-initial.json`
(fixture FPS startup window), `s3-load-report-uart2-cpu-failure.json` (RDM timing)
and `s3-load-report-uart2-timing-abort.json` (fixture timing sample). The runner
now gives the FPS window time to settle, reduces analyzer JSON sampling, records
failed protocol samples and continues other profiles while keeping overall
qualification failed.

Pre-hardening report: `build/hil/s3-load-report-uart2-pre-hardening.json`, **overall failed**. Each
profile ran 30 seconds of input, output and interleaved controller operations:

| Load | DEVICE_INFO ACKs / attempts | Complete scans / attempts | GET/SET errors |
| --- | --- | --- | --- |
| None | 170 / 170 | 34 / 34 | 0 |
| CPU, both cores | 5 / 170 | 34 / 34 | 299 |
| WLAN AP + active scans | 170 / 170 | 34 / 34 | 0 |
| CPU + WLAN | 79 / 169 | 33 / 33 | 210 |

GET/SET errors include failed DEVICE_INFO, IDENTIFY SET and IDENTIFY readback
checks, not just the ACK count in column two. Completed full scans do not
qualify every discovery primitive: the subsequent combined-load discovery
matrix stopped at unicast MUTE with `status: 5, flags: 4`. The combined GET/SET
matrix stopped at the zero-capacity assertion; this is not evidence of a memory
copy bug because that assertion also requires a valid ACK.

The eight 512-slot DMX input/output phases passed. Every 30-second input phase
published 1203..1205 exact-pattern frames without additional receive errors;
output counters advanced by 1197..1203 frames. All 15 existing DMX/fault/recovery/
100-lifecycle cases also passed at combined load. WLAN phases completed 13..14
scans each with no scan failure. CPU work-window fractions were approximately
0.70 on each core; these include preemption and are not actual CPU utilization.

There were zero TX timeouts. Quiescent heap after all load profiles/matrices was
exactly restored: **285768 -> 285768 bytes**. Cumulative RX errors in the final
status include the intentional DMX noise/fault matrix. The fixture is idle/
default and the S3 is UART2 input/verbose with computation/scans/WLAN off.
UART1 and UART2 load-enabled sketches compile; 26 host-runner tests and native
core tests pass. Remote CI was not run for this uncommitted local milestone.

### Interrupt-driven TX-end hardening

RDM request completion now uses `UART_INTR_TX_DONE`, not a foreground observation
of UART-idle. The IRAM handler allows the peripheral FSM two microseconds to
settle, confirms all request bytes were queued and the UART is actually idle,
timestamps request EOP, resets/arms capture and changes DE//RE before publishing
completion to the caller. A preempted caller can then wait in RTOS ticks without
moving the wire-time reference or postponing receiver activation. Broadcast
requests deliberately keep driving MARK; TX failure/cancellation disarms the
pending completion path. No timing acceptance thresholds were changed.

The UART1 word-flag retest passed all functional matrices but recorded one
additional combined-load input error: a 46-us GPIO-only low pulse was treated
as a second BREAK, and FIFO reset lost a zero byte (44 bytes instead of the
declared 45, while the checksum sum happened to remain unchanged). This is a
separate GPIO timestamp/edge-coalescing issue, not the former late TX reference.
The receive adapter now requires UART BREAK/framing corroboration for additional
low-pulse candidates after the leading RDM BREAK (and for BREAK candidates in
BREAK-less discovery). Leading response BREAK duration is still physically
validated; corroborated short/long BREAKs remain invalid. Native tests cover
both fake GPIO candidates and genuine short BREAK candidates. No byte-count,
checksum or timing acceptance rule was relaxed.

BREAK/MAB generation uses a short, bounded critical section (nominal 176+16 us)
also shared by continuous DMX output; IRQs remain enabled for the packet data
and FIFO refill. Direction changes use direct GPIO LL operations, disabling
the receiver before enabling DE and releasing DE before enabling the receiver.
The final flags are word-sized atomics because this Xtensa `-Os` toolchain can
outline `atomic<bool>::load()` into flash. ELF/disassembly checks confirm IRAM
placement and ROM/IRAM calls for the timing-critical handoff routines.

`tests/hil/check_s3_iram.py` checks seven timing/receive routines in the ELF and
rejects flash placement and resolved flash code/data references. CI invokes it
after UART2 compilation. The guard detects the original outlined boolean-load
problem; indirect user callbacks remain subject to the documented ISR contract.
The installed Arduino core's `__onPinInterrupt` dispatcher is still in flash,
although our GPIO callback and the ESP-IDF GPIO service loop are in IRAM. Thus
the guard does **not** certify the complete GPIO dispatch chain during cache-off
flash operations. The direct UART TX_DONE handoff does not use that dispatcher;
cache-off/flash-write receive qualification remains a separate hardware task.

Under AP/active-scan load, asynchronous WLAN result allocations can change live
heap snapshots. Loaded matrices allow a bounded 8192-byte transient variation;
the unloaded default remains 128 bytes. The final load harness shuts WLAN/scans
off, parks the same worker tasks, stops the port and applies a separate **128-byte
quiescent** comparison against the warmed baseline. This distinguishes transient
radio-driver storage from accumulated leaks; protocol assertions remain strict.

### Hardened UART1 retest (2026-10-02)

The final GPIO-corroborated build was flashed and tested with RP2040 firmware
0.4.17. Report: `build/hil/s3-load-report-uart1-final.json`.

| Load | DEVICE_INFO ACKs / attempts | Complete scans / attempts | GET/SET errors |
| --- | --- | --- | --- |
| None | 170 / 170 | 34 / 34 | 0 |
| CPU, both cores | 170 / 170 | 34 / 34 | 0 |
| WLAN AP + active scans | 170 / 170 | 34 / 34 | 0 |
| CPU + WLAN | 170 / 170 | 34 / 34 | 0 |

At combined load all 15 DMX cases, 37 RDM functional checks plus the short-MAB
fixture probe (38 entries), and 24 discovery checks completed successfully.
There were no TX timeouts. Quiescent heap was exactly **284952 -> 284952 bytes**.

The overall report remains **failed**, solely at `combined_output_512`: one
sample reported a **7-us MAB** (nominal generation is 16 us), with a 165-us BREAK
(nominal 176 us). All sampled 512-byte output payloads matched. The immediate
snapshot's completed-frame MAB minimum was still 14 us, but later samples did
include 7 us in the accumulated minimum. The tester updates current BREAK/MAB
fields before publishing completed-frame statistics, so the first discrepancy
alone is not a contradiction; it does not invalidate the later low minimum.
GPIO ISR timestamp latency in the fixture is a possible cause, not an established
explanation. No assertion was weakened and no tester firmware was changed in
this hardening. A calibrated/PIO or oscilloscope capture is needed to distinguish
wire timing from fixture measurement error. This run proves the tested RDM
regression recovery, not complete DMX output timing qualification.

Both final S3 UART sketches compile (903397 flash bytes, 49864 global RAM bytes),
and both ELFs pass the seven-routine IRAM guard. All 37 hardware-free host tests
and the native core suite pass. The ESP8266 controller smoke still compiles;
remote GitHub CI has not run for this uncommitted local milestone.

### Hardened UART2 retest (2026-10-02)

The identical final backend was then built/flashed for UART2 on unchanged pins.
Report: `build/hil/s3-load-report-uart2-final-gpio.json`, **overall passed**.

| Load | DEVICE_INFO ACKs / attempts | Complete scans / attempts | GET/SET errors |
| --- | --- | --- | --- |
| None | 170 / 170 | 34 / 34 | 0 |
| CPU, both cores | 170 / 170 | 34 / 34 | 0 |
| WLAN AP + active scans | 170 / 170 | 34 / 34 | 0 |
| CPU + WLAN | 169 / 169 | 33 / 33 | 0 |

All eight input/output phases and the combined-load DMX (15), RDM (38 entries,
including the short-MAB fixture probe) and discovery (24) matrices passed.
Completed-frame output minima across the four profiles were BREAK 163..167 us
and MAB 11..14 us. No TX timeout; quiescent heap **284952 -> 284952 bytes**.
This successful run does not erase the UART1 timing anomaly or qualify
simultaneous two-port operation.

After these full hardware runs, the runner gained a stricter check of
completed-frame timing minima in addition to live fields, with five new dry
tests. Applying that check to every saved output sample preserves UART2's pass
and UART1's failure; these saved-sample checks are not a new hardware run.

### Independent PIO/DMA investigation (2026-10-02)

RP2040 tester **0.4.18** adds a finite raw digital sampler, separate from the
old GPIO-edge analyzer: two PIO instructions per one-bit sample, 4 MHz, DMA into
a fixed 64-KiB buffer, 131.072-ms windows. Decoding occurs afterwards; runs
touching either window boundary are excluded. PIO FIFO stalls, DMA timeouts and
incomplete captures invalidate a window. The sampler is passive on GPIO1 and
runs only with the fixture idle, not concurrently with its byte analyzer or
RDM responder. No S3 timing code was changed for this investigation.

A second PIO state machine generates 176-us BREAK pulses on the tool's own
GPIO0, followed by 16-, 8- or 7-us high pulses. The sampler observes the same pin
without a jumper. More than 400 complete pairs per case measured exactly the
configured values; all 7-us MABs were classified short. Native C++ tests cover
raw-bit packing, word boundaries, partial captures, threshold/short-MAB cases,
UART data pulses and bounded detail storage. The reference and sampler share
one RP2040 clock: this is not absolute timebase calibration.

`run_s3_pio_timing.py` then ran each UART separately with the same four 30-second
load profiles and 512-slot output. Both reports pass:

| Load | UART1 complete pairs | UART2 complete pairs | MAB range, either UART |
| --- | --- | --- | --- |
| None | 677 | 679 | 16.25..16.50 us |
| CPU, both cores | 677 | 677 | 16.25..16.75 us |
| WLAN AP + active scans | 670 | 673 | 16.25..16.50 us |
| CPU + WLAN | 667 | 674 | 16.25..16.75 us |

Each profile had 129 valid windows, covering **16.908288 seconds of its roughly
30-second interval**. All **5394** complete pairs had MAB >=16.25 us; minimum
BREAK was 176 us in each profile. No stall, timeout or short MAB. Quiescent S3
heap was exactly restored: UART1 **284980 -> 284980**, UART2 **284952 -> 284952**.
Generated reports: `build/hil/s3-pio-timing-report.json` and
`build/hil/s3-pio-timing-report-uart2.json`. The original failed GPIO-analyzer
report remains unchanged.

These results favor ISR timestamp jitter in the old fixture over a real short
S3 output MAB; the exact historical frame was not recorded by this independent
sampler, so that explanation is an **inference**, not retrospective proof.
Capture/decode/USB gaps prevent a continuous worst-case guarantee. Analog edges,
RS485 levels/contention and payload validation are outside this measurement.
The raw reference's 7-us pulse is not a complete RDM-response stimulus: the
earlier short-MAB responder qualification gap remains.

All 42 hardware-free NocteDMX host tests pass. The tester builds on the pinned
PlatformIO/Arduino-Pico toolchain (80856 RAM bytes, 132724 flash bytes) and was
flashed locally. Six existing standalone RP2040 regression functions also pass.
After the raw-window runs, all 15 normal DMX/fault/lifecycle cases, all 38 RDM
entries and all 24 discovery entries passed again with tester 0.4.18. This
includes 100 port lifecycle cycles, 100 foreground and 100 host-paced GETs,
20 full scans and conformant recovery after malformed traffic. The fixture
returned to idle/default and the S3 to UART1 input/verbose with load/WLAN off.

### ESP8266 follow-up assessment (historical, before its hardening)

The ESP8266 controller uses a different, synchronous FIFO path: it waits for
FIFO-empty, estimates request EOP as final-slot start +44 us, delays 60 us, then
arms/releases receive. It has no equivalent S3 FreeRTOS competing-task issue,
but interrupt latency can still delay that observation/turnaround. Its
BREAK/MAB and final FIFO/shift-register drain merit separate bounded hardening
and WLAN/IRQ-load tests on actual ESP8266 hardware. Do not copy the S3 TX_DONE
patch blindly: the register/interrupt facilities differ. No ESP8266 behavior
was changed in this S3 hardening; the existing controller smoke sketch still
compiles on ESP8266 core 3.1.2.

The subsequent ESP8266 follow-up is now implemented and tested: short IRAM
framing/final-slot handoff windows, bounded FIFO waits, explicit TX failure
recovery and a forced-inline interrupt lock. Four 20-second IRQ/foreground/
AP-scan profiles pass with cumulative RDM failure flags (94 DEVICE_INFO cycles,
Identify SET/read-back/restore, maximum-PDL SET/NACK and 18 discovery cycles),
zero TX timeouts and exact quiescent heap recovery. Independent output capture
passes 173 windows/961 pairs; normal DMX input/output and RDM fault/recovery
smoke tests pass again. See the [ESP8266 HIL scope](../tests/hil/README.md#esp8266-synthetic-load-and-independent-output-timing)
for the final-slot timestamp estimate, fixture limitations and remaining
RS485/NMI/cache-off/external-UDP qualification. This does not alter the S3 results.

### Remaining qualification

- RS485/worst-case qualification for S3 discovery/Mute/Unmute; responder operation.
- ACK_TIMER_HI_RES, automatic queued-message handling and overflow aggregation.
- PIO-framed complete RDM response stimuli with true short MAB and exact timing
  boundaries (the new digital sampler/reference self-test is not that responder).
- Maximum response PDL on physical hardware (native maximum-size tests exist).
- RS-485 transceivers, DE//RE timing, bus bias/termination and contention.
- Longer/continuous independent capture if a worst-case guarantee is required;
  the old 7-us sample is not reproduced in the PIO/DMA windows above.
- Sustained external UDP traffic, prolonged CPU/Wi-Fi soak, cache-off/flash-write
  receive stress and simultaneous multi-port operation.

The local JSON reports are generated under `build/hil/` (not versioned).
Cleanup leaves RP2040 idle/default RDM settings and S3 input/verbose.
