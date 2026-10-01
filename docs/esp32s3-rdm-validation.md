# ESP32-S3 UART1 RDM controller validation

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

## Remaining work

- S3 discovery/Mute/Unmute, broadcast sequencing and responder operation.
- ACK_TIMER_HI_RES, automatic queued-message handling and overflow aggregation.
- A calibrated/PIO fixture path for true short MAB and exact boundary stimuli.
- Maximum response PDL on physical hardware (native maximum-size tests exist).
- RS-485 transceivers, DE//RE timing, bus bias/termination and contention.
- Wi-Fi/CPU/cache stress, UART2 and multi-port operation.

The local JSON reports are generated under `build/hil/` (not versioned).
Cleanup leaves RP2040 idle/default RDM settings and S3 input/verbose.
