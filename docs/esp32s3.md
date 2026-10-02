# ESP32-S3 UART bring-up

This is an **experimental, UART1/2-tested DMX input/output and RDM controller backend**.
The [first hardware validation](esp32s3-validation.md) covers 15 cases with
direct UART. It is not a standards-conformance or electrical qualification.
Typed unicast RDM GET/SET is available (`supportsRdmController == true`).
Discovery/Mute/Unmute is implemented (`supportsRdmDiscovery == true`), with
native and UART1/2 direct-UART coverage. Full qualification and responder operation
is not yet available; `supportsRdm` remains
false for the full legacy RDM surface. No standards certification is implied.
See [the RDM validation record](esp32s3-rdm-validation.md) for results and limits.

## Direct UART bench wiring

| ESP32-S3 | RP2040 tester | Purpose |
| --- | --- | --- |
| GPIO17 | GPIO1 | S3 TX to tester RX |
| GPIO18 | GPIO0 | S3 RX from tester TX |
| GND | GND | Shared reference |

Only 3.3-V logic devices may be connected this way. **Do not connect these
pins to DMX A/B or to 5-V UART outputs.** No RE/DE wiring is needed; the bench
sketch uses `setDirectionPin(255)`. The tester's unused direction GPIO can
remain unconnected. Stop the tester's transmission before testing S3 output.

GPIO17/18 are exposed on the official
[ESP32-S3-DevKitC-1](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html).
Check the schematic of other boards for peripherals already occupying them.
They avoid the native USB GPIO19/20 and the UART0 console pins.

## Dry build

Install Arduino CLI and Espressif Arduino Core **3.3.12**, then run from the
library root:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  --library . --warnings all --build-path build/s3/Esp32S3UartBench \
  extras/hil/Esp32S3UartBench
```

This only builds; it does not flash a board. USB CDC on boot is required for
the console. The generic board definition uses 4-MB flash and needs no PSRAM;
select a matching board configuration if your board requires other options.

The bench starts as a receiver. Send newline-terminated commands over USB:

- `input`: receive DMX on GPIO18 and report the last complete frame.
- `output`: transmit 512 channels on GPIO17, `(channelIndex * 17 + 3) & 255`
  with zero-based channel index, at nominal 40 Hz.
- `output24`: the same pattern with 24 channels.
- `rdm`: continuous DMX with foreground RDM controller GET/SET enabled.
- `get HEX_PID [capacity]`: query the synthetic fixture UID `7FF0:52444D01`.
- `set HEX_PID HEX_DATA`: send parameter bytes (e.g. `set f0 002b` for address 43).
- `invalid` / `burst`: argument guards / 100 back-to-back DEVICE_INFO reads.
- `branch LOWER_UID UPPER_UID`: inclusive discovery branch, UIDs as 12 hex digits.
- `mute UID` / `unmute UID`: unicast or all/manufacturer broadcast.
- `scan [transactionLimit]`: full discovery, 16-device table, default 512 transactions.
- `stop`: release the UART and stop traffic.
- `status`: report mode, active state, setup error, slot count, error counters
  and free heap.
- `frame`: return a JSON snapshot of all received channels.
- `quiet` / `verbose`: disable/enable automatic receive reports.

The RX callback only signals frame availability. Reporting and snapshots run
in foreground context, not in the ISR.

## Automatic hardware tests

The host runner can build and **flash** DMX smoke sketches for S3:

```sh
python tests/hil/run_smoke.py --chip esp32s3 --tests dmx \
  --esp-port S3_SERIAL_PORT --fixture-port RP2040_SERIAL_PORT
```

Do not run this until the physical setup and programming port are ready.
Use a current esptool (4.x or 5.x). The runner writes the generated merged
image at offset 0, including bootloader/partition table; it does not incorrectly
write the application-only image there. Native USB can re-enumerate when
switching between bootloader and firmware; use the board's USB-UART bridge if
necessary for a stable programming port. The standalone interactive bench is
an alternative for first bring-up. S3 input/output have passed with this path;
`--tests all`/`rdm` are rejected before touching serial ports.
`--esptool-executable` can select the vendor's standalone executable without
installing another Python esptool package.

Run the expanded bench suite with tester firmware **0.4.15 or later**:

```sh
python tests/hil/run_s3_bench.py --esp-port S3_SERIAL_PORT \
  --fixture-port RP2040_SERIAL_PORT
```

This builds/flashes the interactive bench, tests receive slot counts, legal
channel pauses, invalid start codes/BREAKs, noise recovery, full output,
console activity and 100 lifecycle cycles. `--no-flash` uses the already loaded
bench. The tester is returned to idle/normal timing and the S3 to input/verbose.
Tester 0.4.14 overwrites configured start codes before sending; the runner
checks what the fixture actually sends. `--skip-start-code` explicitly records
that coverage gap instead of treating it as a successful rejection test.

Run the dedicated S3 RDM matrix separately (it uses the same bench sketch):

```sh
python tests/hil/run_s3_rdm.py --esp-port S3_SERIAL_PORT \
  --fixture-port RP2040_SERIAL_PORT
```

`--no-flash` reuses the loaded sketch. This tests DEVICE_INFO, bounded copies,
SET/readback, NACK, maximum request size, timing/envelope faults, recovery and
repeated commands. It records actual timing estimates and a coverage gap when
the fixture cannot physically generate a sub-8-us MAB. Cleanup independently
returns the tester to idle/default RDM settings and S3 to input/verbose.

### Select UART1 or UART2 without rewiring

The bench defaults to UART1. All three S3 runners accept `--uart 1` or `--uart 2`;
both route TX to GPIO17 and RX to GPIO18. For a UART2 comparison:

```sh
python tests/hil/run_s3_bench.py --uart 2 --esp-port S3_SERIAL_PORT \
  --fixture-port RP2040_SERIAL_PORT
python tests/hil/run_s3_rdm.py --uart 2 --no-flash --esp-port S3_SERIAL_PORT \
  --fixture-port RP2040_SERIAL_PORT
python tests/hil/run_s3_discovery.py --uart 2 --no-flash --esp-port S3_SERIAL_PORT \
  --fixture-port RP2040_SERIAL_PORT
```

The first command builds/flashes; the others reuse that exact bench. Status JSON
includes `uart`, and the runner rejects a missing or different UART before running
the matrix. `--no-flash` never switches the peripheral by itself. Separate build
directories and UART2 report suffixes preserve UART1 artifacts. Manual compilation
can set `compiler.cpp.extra_flags=-DNOCTE_HIL_UART_NUMBER=2`; do not replace the
core's `build.extra_flags` because they contain required native-USB definitions.
CI compiles the bench for both UARTs. This is an alternative peripheral on the
same pins, not a simultaneous two-port or GPIO-ownership qualification.

### Two simultaneous instances: supported architecture, pending validation

The backend already has per-UART ownership, GPIO conflict checks, per-instance
buffers/locks/interrupt state and a separate output task for each transmitting
port. Two instances can select UART1 and UART2 with distinct TX/RX and direction
pins; constructing two default ports instead selects UART1 twice and the second
start is rejected. Each physical DMX bus needs its own RS485 transceiver.

This is not an additional implemented feature milestone: the architecture is
present, but the tested UART1/2 matrices above use one peripheral at a time.
The simultaneous-port test is deferred. It should cover different 512-slot
patterns, independent start/stop, mixed input/output, resource conflicts and
RDM on one port while the other continues DMX, followed by combined-load and
timing checks. Do not claim two-port hardware qualification before that test.

## RDM controller contract

Use `startRDM(255)` for this direct-UART bench, or tied/split direction pins with
real transceivers. Only `PortMode::Send` is implemented. An unsupported receive
mode leaves the port stopped with `InvalidConfiguration`. Configure your UID
with `setUid`; the default `7FF0:00000002` is experimental, not a production UID.
Calls are blocking and must originate from the same foreground task as lifecycle
operations. DMX pauses only at a frame boundary and resumes after the response
window. GET optionally accepts request PDL and a sub-device; SET accepts a
sub-device. Broadcast GET/SET is rejected by these typed helpers.

The portable receiver checks physical BREAK/MAB estimates, SOP spacing, per-byte
interval and total packet duration, structural fields, checksum and correlation.
It waits a 2144-us quiet window before closing, conservatively rejecting extra
bytes in that window (unlike the ESP8266's existing post-EOP discard policy).
Timeouts remain bounded; no automatic ACK_TIMER retry, queued-message polling,
or ACK_OVERFLOW aggregation is performed. ACK_TIMER_HI_RES is not implemented.
`copyRdmResponse` and `rdmReceiveTiming` expose the last captured packet/estimates
for diagnostics; inspect the result status before interpreting its data.

### Discovery controller API

`discoverRdmBranch(lower, upper, &uid)` broadcasts DISC_UNIQUE_BRANCH for an
inclusive UID range. It returns `None`, `CollisionOrMalformed`, or `SingleDevice`.
The fixed 32-byte discovery receiver observes any GPIO/UART activity, accepts
0..7 FE preamble bytes and checks masks/checksum, SOP spacing and packet duration.
Even activity without a decoded UART byte is a possible collision, never `None`.
Capture waits at least 5800 us after the request and 176 us after the last slot;
a 10-ms watchdog bounds pathological continuous traffic. Invalid arguments or
transport failure return `CollisionOrMalformed`; this is not a retry policy.

`setRdmDiscoveryMute(target, mute)` sends DISC_MUTE or DISC_UN_MUTE. Unicast ACK
requires a 2- or 8-byte control field payload, with reserved flags zero. Deferred
and overflow replies are rejected. Broadcast returns `RdmCommandStatus::Sent`
(not ACK and `ok()` is false); the driver keeps MARK active because no response
is expected. Normal typed GET/SET still rejects broadcast destinations.

```cpp
nocte::dmx::core::DeviceTable<32> devices;
auto scan = nocte::dmx::core::scanRdmDevices(port, devices, 512);
if (scan.status == nocte::dmx::core::RdmScanStatus::Complete) {
  // Read devices.get(index, uid); muted devices still answer normal GET/SET.
}
```

The portable full-scan helper has a fixed 49-range DFS stack, no heap/recursion,
and a transaction budget including UNMUTE/MUTE. It unmutes first, confirms UIDs
by MUTE ACK, repeats the same branch after muting, splits collisions, and tries
an exact-UID MUTE at a malformed leaf. Table exhaustion, budget exhaustion and
unresolved leaves are explicit non-complete results. Results can be partial;
devices remain muted after a scan. The helper requires a transport implementing
these two primitives; the ESP8266 legacy scan API is unchanged.

Run `tests/hil/run_s3_discovery.py` with tester **0.4.17 or later** and the same port/programmer arguments as
the GET/SET runner. Its 24 checks cover ranges, mute scope, GET while muted,
malformed replies/recovery, budgets and 20 repeated scans. Native tests simulate
multiple devices/collisions; one direct-UART responder cannot qualify electrical
collisions. See the validation record for current hardware status.

## Backend contract and limitations

- Defaults: UART1, TX17/RX18. A constructed port can select UART2 and other
  valid pins; UART0 is reserved. Configure before starting, check `isActive()`
  and `lastError()` afterwards. Do not share the UART/GPIOs with other drivers.
- Foreground lifecycle calls must come from one task. Calling `stop()` from
  an ISR/callback is not supported. Frame APIs use a per-port spinlock.
- Port storage and callbacks must be ISR-safe/internal RAM/IRAM; no PSRAM
  placement for the port object. Do not log, allocate or block in callbacks.
- Start allocates the output task; no steady-state data-path allocation.
- RDM EOP timestamp, capture activation and transceiver turnaround are handled
  in the UART TX_DONE ISR, including a hardware-idle check, not after task wakeup.
  BREAK/MAB have a bounded critical section; packet data uses interrupt-driven
  FIFO refill. See the validation record for load-test scope and exclusions.
- Output uses 250000 baud, 8N2, nominal 176-us BREAK and 16-us MAB, default
  40 Hz (configurable 1..44 Hz). A dedicated TX snapshot keeps updates from
  changing a partly transmitted frame and is copied **before BREAK**, so its
  length does not extend MAB. Hardware-idle plus all queued bytes
  determines completion, not merely FIFO empty.
- Receive uses UART byte/error interrupts and GPIO edges to qualify a BREAK
  low period of at least 88 us. This initial GPIO-edge approach adds CPU load
  and needs stress/timing measurement before any production claim.
- The portable receiver publishes at the next qualified BREAK. This adds a
  frame boundary of latency and leaves a final isolated frame unpublished;
  it avoids treating legal inter-slot pauses as packet boundaries. A BREAK
  closes a short frame; an arbitrary transport interruption has no independent
  indication of the intended slot count.
- Unknown start codes, packets longer than 512 channels and observed UART
  errors discard the incomplete packet and keep the last good snapshot.
  MAB and all timing boundaries are not yet qualified by this initial receiver.
- RS-485 direction pins can be configured for later transceiver tests, but
  direct UART cannot validate line release, electrical contention or biasing.

## Next hardware steps

`tests/hil/run_s3_load.py` supplies repeatable synthetic dual-core computation
and WLAN AP/active-scan profiles, with 512-slot input/output and GET/SET/discovery.
It records failures rather than treating unloaded passes as stress qualification.
The [RDM validation record](esp32s3-rdm-validation.md) documents the original
CPU-load TX-end issue, the interrupt-driven fix and separate retest results.
A sampled output-MAB anomaly now has a separate PIO/DMA retest; this is not a full
production timing qualification. AP/scans are not sustained external UDP traffic.

1. Check idle level, 24/512-channel output and full-frame input against the RP2040.
2. Measure BREAK/MAB, refresh, inter-slot gaps and last-stop-bit completion.
3. Probe short/long BREAK, errors, oversized packets, recovery and start/stop.
4. Repeat under USB/CPU/Wi-Fi load; check GPIO/UART interrupt ordering at BREAK.
5. Repeat controller/discovery tests under load and with simultaneous independent ports;
   responder support is separate. UART1 and UART2 have passed separately on direct UART.
6. Qualify RDM with real RS-485 transceivers, direction control and collisions.
