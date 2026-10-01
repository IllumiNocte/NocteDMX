# ESP32-S3 UART bring-up

This is an **experimental, UART1-tested DMX input/output and RDM controller backend**.
The [first hardware validation](esp32s3-validation.md) covers 15 cases with
direct UART. It is not a standards-conformance or electrical qualification.
Typed unicast RDM GET/SET is available (`supportsRdmController == true`).
Discovery and responder operation are not yet available; `supportsRdm` remains
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

## Backend contract and limitations

- Defaults: UART1, TX17/RX18. A constructed port can select UART2 and other
  valid pins; UART0 is reserved. Configure before starting, check `isActive()`
  and `lastError()` afterwards. Do not share the UART/GPIOs with other drivers.
- Foreground lifecycle calls must come from one task. Calling `stop()` from
  an ISR/callback is not supported. Frame APIs use a per-port spinlock.
- Port storage and callbacks must be ISR-safe/internal RAM/IRAM; no PSRAM
  placement for the port object. Do not log, allocate or block in callbacks.
- Start allocates the output task; no steady-state data-path allocation.
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

1. Check idle level, 24/512-channel output and full-frame input against the RP2040.
2. Measure BREAK/MAB, refresh, inter-slot gaps and last-stop-bit completion.
3. Probe short/long BREAK, errors, oversized packets, recovery and start/stop.
4. Repeat under USB/CPU/Wi-Fi load; check GPIO/UART interrupt ordering at BREAK.
5. Add S3 discovery/Mute/Unmute on the bounded controller transport, then responder support.
6. Qualify RDM with real RS-485 transceivers, direction control and collisions.
