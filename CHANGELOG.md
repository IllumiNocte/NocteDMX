# Changelog

## 0.1.0 - Unreleased

- Hardened ESP8266 controller BREAK/MAB and reserved-final-byte/RX handoff in
  short IRAM windows with forced-inline PS restoration. Bulk FIFO work remains
  interruptible, with explicit timeout/recovery counters and partial-discovery
  reporting on TX failure. Legacy DMX scheduling and protocol limits remain
  unchanged; EOP remains a final-slot estimate, not S3-style TX_DONE capture.
- Added an independent ESP8266 four-profile IRQ/foreground/AP-scan load bench,
  finite stress sweeps, cumulative fault checks, separated build variants and
  host rejection tests. Extended the ELF IRAM guard/CI to the ESP8266, including
  the core interrupt-lock destructor outlining regression.
- ESP8266 at 80 MHz passes output PIO/DMA measurement with tester 0.4.18:
  173 windows, 961 complete pairs, BREAK 102.25..102.5 us, MAB 44.25..48 us,
  no short pulses/stalls/timeouts and exact quiescent heap recovery. Hardware
  runner/core tests pass (54 host tests); finite-window, console/responder,
  physical RS485, cache-off/NMI and external-UDP limitations remain explicit.
- ESP8266 RDM passes all four 20-second profiles with cumulative failure flags:
  94 DEVICE_INFO cycles, Identify SET/read-back/restore, 94 maximum-PDL SET/NACK
  checks and 18 discovery/Mute cycles; zero failure flags/TX timeouts and
  quiescent heap 48032 -> 48032 bytes. The console recovery interval is required
  for a meaningful final 512-slot guard, not a relaxation of wire timing limits.

- Added independent S3 PIO/DMA output timing windows using RP2040 tester 0.4.18,
  hardware reference/short-MAB self-tests, load telemetry and host rejection
  tests. The finite capture excludes incomplete boundary pulses and rejects
  PIO/DMA stalls; no GPIO IRQ timestamps or relaxed timing assertions.
- Both UARTs pass the four-profile PIO/DMA output retest: 5394 complete framing
  pairs, MAB 16.25..16.75 us, no short MAB/stall/timeout. The historical GPIO-ISR
  7-us sample was not reproduced; finite windows and shared-clock calibration
  limits remain explicit. Native decoder tests and 42 host tests pass.

- Hardened S3 RDM request EOP/receive/DE//RE handoff through UART TX_DONE in
  IRAM, with actual FIFO/FSM-idle confirmation, bounded BREAK/MAB protection,
  direct GPIO writes, explicit timeout disarming and word-sized ISR flags.
  Public protocol/API behavior and acceptance thresholds are unchanged.
- Corroborate additional RDM GPIO low-pulse candidates with UART BREAK/framing
  status, preventing timestamp jitter from flushing valid response bytes; true
  short/long BREAKs still fail strict validation. Added native pulse regressions
  and an ELF IRAM/flash-dependency build guard with host tests and CI integration.
- Added delayed-foreground timestamp core regressions and bounded WLAN transient
  heap accounting with a tighter separate 128-byte quiescent leak check.
- Final hardened UART1/2 load retests have no GET/SET/discovery protocol errors;
  all combined-load functional matrices pass, no TX timeout and exact quiescent
  heap recovery. UART2 passes overall; UART1 retains one sampled 7-us output-MAB
  failure requiring independent timing capture. Output tests also check
  completed-frame minima so intermittent failures cannot hide between samples.
  Native tests and 37 host tests pass; full electrical/cache-off qualification
  and ESP8266-specific hardening remain separate work.

- Added a standalone S3 dual-core CPU/AP-scan load runner, explicit load
  telemetry, host-side rejection tests and load cleanup. Initial CPU testing
  exposed RDM TX-end timestamp/turnaround sensitivity; documented the failure
  without relaxing protocol timing checks or claiming loaded qualification.
- Pre-hardening UART2 load results: all eight 30-second full-frame DMX input/output phases
  and the 15-case DMX matrix pass; RDM passes without CPU load and with WLAN
  AP/scans alone, but fails CPU/combined profiles and follow-up RDM matrices.
  Zero TX timeouts, exact quiescent heap recovery, 26 host dry tests pass.

- Made S3 HIL UART selection explicit (`--uart 1/2`, TX17/RX18 unchanged), with
  firmware-status verification, separate builds/reports, dry tests rejecting
  mismatched firmware, and UART2 bench compilation in CI.
- Repeated the direct-UART matrices on S3 UART2 with tester 0.4.17: 15 DMX
  cases, 37 RDM checks plus the short-MAB fixture probe, and 24 discovery checks
  pass. Heap is unchanged across repeated commands/scans and 100 lifecycle
  cycles; UART1 remains the default. No UART backend changes were needed.

- Added S3 DISC_UNIQUE_BRANCH and Mute/Unmute with bounded BREAK-less capture,
  GPIO-only collision detection, strict discovery encoding/control-field checks,
  and no-response broadcast MARK sequencing (`Sent`, never a fabricated ACK).
- Added a portable fixed-stack full-discovery helper with explicit capacity,
  transaction-budget and unresolved-device results, plus native multi-device
  collision/leaf tests and a dedicated 24-point direct-UART discovery HIL matrix.
  Fixed tester edge pairing/early-DUB deadlock (tester 0.4.17 required); documented
  variable early-response fixture timing instead of claiming an unmeasured fault.

- Added S3 unicast RDM controller GET/SET with frame-boundary DMX pause/resume,
  persistent output task, bounded receive deadlines and full UART-idle release.
- Added a portable normal RDM response receiver with fixed storage and native
  tests for timing boundaries, timestamp wraparound, maximum packet duration,
  envelope correlation, extra bytes and malformed traffic.
- Shared typed request construction/response classification with ESP8266;
  added explicit controller/discovery/responder capability flags.
- Added S3 RDM fault/recovery HIL and diagnostic timing reports; documented the
  tester's short-MAB coverage gap and remaining S3 discovery/responder/RS485 work.

- Hardware-smoke-tested ESP32-S3 UART1 against RP2040 tester 0.4.15: 15
  input/output, invalid-start-code/BREAK, noise-recovery, console-activity and
  lifecycle cases, including unchanged stopped-state heap after 100 cycles.
- Moved S3 output snapshots before BREAK so full-frame copying cannot extend
  MAB. Added full JSON snapshots/heap reports to the interactive bench and
  a repeatable expanded hardware runner with explicit fixture coverage guards.
- Added standalone esptool executable support with host-side dry tests.
- Added an experimental ESP32-S3 UART1/2 backend for DMX input/output, with
  configurable pins, optional direction GPIOs, per-frame TX snapshots,
  hardware-idle completion checks and cooperative task shutdown.
- Added portable DMX receive assembly and native tests for unknown start
  codes, truncated/oversized packets, UART errors and recovery.
- Added a direct-UART S3 bench sketch (TX17/RX18, no DE//RE), S3 compile CI
  and chip-aware DMX smoke-test preparation. S3 RDM and hardware timing
  qualification are not implemented/claimed by this milestone.
- Expanded package metadata and DMX example compilation to ESP32-S3 while
  preserving the ESP8266 backend and its full RDM examples.
- Added Arduino-independent UID, fixed-capacity device table, frame storage,
  and RDM transaction storage with native regression tests.
- Added per-instance RDM identity and exclusive ESP8266 UART0 ownership;
  destroying an inactive port no longer stops another port.
- Added typed, bounded RDM GET/SET results and response correlation checks
  for UID, transaction, command class, PID, sub-device, and PDL.
- Fixed legacy `UID::copyToUID` to update the caller's object, UID formatting
  to retain leading zeros, and the missing full-width `rdmPacketLength` getter.
- Corrected UART ISR signatures and preserved prior interrupt state in
  whole-frame operations.
- Unified foreground RDM transmission through the hardware FIFO, with DMX
  frame-boundary handoff, avoiding the older byte-per-interrupt transmit path.
- Fixed 255-byte RDM message lengths wrapping while copying responder packets.
- Added standalone ESP8266/RP2040 HIL sketches and a JSON-reporting runner;
  CI also compiles HIL sketches and checks the runner's Python syntax.
- Documented the current backend contract and the remaining shared-engine
  extraction needed for ESP32-S3.

- Added standalone native tests for the portable DMX/RDM core and GitHub
  Actions jobs for core tests, Arduino library linting, and compilation of all
  ESP8266 examples.

- Renamed the uNode-maintained library to NocteDMX.
- Added the canonical `<NocteDMX.h>` public entry point.
- Added `nocte::dmx::Port` and `nocte::dmx::defaultPort()`.
- Isolated the existing ESP8266 implementation below
  `src/nocte/backends/esp8266`.
- Retained the historical ESP8266 headers, class, and global object as a
  compatibility layer.
- Added Arduino and PlatformIO package metadata for the new name.
- Added an Arduino-independent `nocte/core` layer for shared protocol limits,
  DMX frame operations, RDM packet headers, discovery decoding, and controller
  response validation.
- Kept critical sections and all UART/GPIO/timing mechanics inside the
  ESP8266 backend.
- Reworked the bundled sketches to use the public NocteDMX facade and safe
  whole-frame operations.
- Added a typed `PortMode` for selecting the initial bidirectional direction.
- Removed the historical dual-UART sketch and its private duplicate driver;
  multiple ports will return through the regular backend interface.

This release builds on the preceding uNode-maintained LXESP8266DMX fork while
hardening response handling and establishing the structure for more backends.
