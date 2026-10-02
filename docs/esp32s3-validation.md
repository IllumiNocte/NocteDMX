# ESP32-S3 UART1 first hardware validation

Date: **2026-10-01**. Result: **15 expanded bench cases passed, zero skipped**,
plus the basic DMX output/input smoke groups. These are functional regression
tests and sampled timing observations, not DMX512-A certification.

## Setup

- ESP32-S3 revision v0.2, native USB Serial/JTAG (COM6), Arduino Core 3.3.12.
- RP2040 tester (COM3), firmware **0.4.15**, UART TX0/RX1.
- Crossed 3.3-V UART: S3 TX17 to RP RX1, S3 RX18 from RP TX0, common GND.
- No RS-485 transceiver, RE/DE, Wi-Fi traffic or PSRAM placement of the port.
- S3 sketch: `extras/hil/Esp32S3UartBench`; host: `tests/hil/run_s3_bench.py`.

## Passing cases

- Input: exact complete-frame comparisons with 1, 24, 511 and 512 channels.
- Input: 1-ms inter-slot pauses on a 24-channel frame and a 1000-us BREAK.
- Rejection: nonzero start code `0xCC`, 64-us BREAK and an empty packet do
  not overwrite the last valid frame; unknown start-code traffic stops
  incrementing published-frame counts. The tester's reported start code is
  checked before judging receiver behaviour.
- Recovery: 200 ms of noise with 2..70-us pulses, followed by exact clean
  512-channel reception.
- Console activity: 42 full-frame/status snapshots over 5 seconds, with 196
  additional published full frames and **no new UART errors**. The cumulative
  error count includes the preceding intentionally malformed traffic/noise.
- Output: every channel in 24- and 512-channel deterministic patterns matches.
- Lifecycle: 100 output/stop/input/stop cycles after warm-up, followed by
  successful full-frame reception; zero TX timeouts.

## Observations after timing refinement

The TX frame snapshot was moved before BREAK. Previously the 512-channel
copy extended observed MAB to approximately 50 us. Final sampled values:

| Pattern | Refresh snapshot | BREAK | MAB | Baud estimate |
| --- | --- | --- | --- | --- |
| 24 channels | 40 Hz | 177 us | 18 us | 249546 |
| 512 channels | 39 Hz | 177 us | 18 us | 249967 |

The backend uses nominal 250000 baud, 176-us BREAK, 16-us MAB and 40-Hz refresh.
RP2040 measurements include analyzer/main-loop scheduling uncertainty and are
not a lossless oscilloscope/logic-analyzer capture or guaranteed min/max bounds.

Stopped-state free heap before/after 100 cycles: **343884 / 343884 bytes**.
This detects leakage in this repeated lifecycle, not every allocation/error path.

## Fixture correction

Tester 0.4.14 accepted `set/frame.startCode` but reset it to zero in its pattern
update immediately before transmission. This first looked like an S3 receiver
bug. Tester 0.4.15 preserves the configured start code; the S3 then correctly
rejects the actual `0xCC` frames. A source regression check accompanies the
tester fix in the consuming uNode repository. Do not claim alternate-start-code
coverage from the unmodified 0.4.14 transmitter.

## UART2 comparison (2026-10-02)

The same GPIO17 TX / GPIO18 RX wiring and tester **0.4.17** were used with
`run_s3_bench.py --uart 2`. The bench explicitly reports `uart: 2`; all **15
DMX cases passed**, including 100 lifecycle cycles. No backend code change was
required. UART1 remains the default, with a successful default build regression.
Nineteen hardware-free host tests pass, including UART selection, isolated build
paths/report names, invalid UART rejection and stale/wrong-firmware detection.

- Exact 24/512-channel output and 1/24/511/512-channel input match as on UART1.
- The 5-second console activity test returned 43 snapshots and 200 additional
  complete input frames, with no additional receive errors.
- Sampled output BREAK/MAB: 177/18 us for both lengths; baud estimates
  250227.5 (24 slots) and 249988.9 (512 slots). These are fixture estimates,
  not calibrated bounds; earlier malformed/noise traffic remains in counters.
- Stopped-state heap across 100 cycles: **341244 -> 341244 bytes**; zero TX timeouts.
- The [RDM/discovery matrices](esp32s3-rdm-validation.md) also pass on UART2.

Report: `build/hil/s3-bench-report-uart2.json` (generated, not versioned).
After cleanup the S3 remains **UART2 input/verbose**, tester idle. This compares
one peripheral at a time, not simultaneous UART1+UART2 operation.

## Remaining qualification

The repeatable synthetic load runner is `tests/hil/run_s3_load.py` (same port,
programmer and `--uart` arguments). It compares no load, CPU-only, Wi-Fi AP plus
active scans, and combined load in 30-second input/output/RDM phases, then
repeats the existing fault matrices at combined load. Worker/scan progress is
verified; busy-window time is not calibrated CPU utilization. External UDP
payload throughput, flash/cache worst cases and electrical timing remain outside
this scope. See the [RDM validation record](esp32s3-rdm-validation.md) for the
original CPU-load failure, interrupt-driven hardening and final retest results;
unloaded passes alone are not a load qualification.

The **pre-hardening** UART2 load report passes all eight 30-second 512-slot DMX input/output
phases and all 15 existing DMX cases at combined load. Input: 1203..1205 complete
frames per phase, no new receive errors. Output: 1197..1203 transmitted frames
per phase; 30 sampled timing/pattern snapshots per profile. No TX timeout, and
quiescent heap restored exactly (285768 -> 285768 bytes). The overall load report
is nevertheless **failed**, because normal RDM transactions are unreliable
under CPU load. This historical failure is not the hardened controller's result.
WLAN AP/scans alone passed the tested RDM operations. Real UDP
payloads, longer soak, exact timing and RS485 qualification remain outstanding.

- Independent finite PIO/DMA output retests now pass on UART1/2 under the four
  load profiles; see the RDM validation record for counts, sampling gaps and
  the original GPIO timing anomaly. No S3 code/threshold changed for this retest.
- Full timing boundaries, short MAB validation, framing/parity/overflow faults
  and prolonged worst-case GPIO interrupt/CPU/Wi-Fi load.
- More than 512 input channels on hardware (the current fixture TX API clamps
  at 512); the portable receiver's oversize rejection has native tests only.
- Simultaneous independent ports, pin conflicts and setup failures on
  hardware, plus lifecycle calls in other supported task configurations.
- RS-485 electrical levels, termination/bias, DE//RE release and contention.
- Responder operation and full RDM conformance. The subsequent
  [GET/SET controller milestone](esp32s3-rdm-validation.md) now has separate
  functional/fault/recovery coverage and also reruns these 15 DMX cases.

After cleanup the RP2040 is idle with normal timing/start code and the S3
remains in input mode with verbose console reporting enabled.
