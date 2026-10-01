# ESP32-S3 UART bring-up

This is an **experimental, compile-tested DMX input/output backend**. No S3
hardware test or standards-conformance claim accompanies this milestone.
RDM is deliberately unavailable (`Port::supportsRdm == false`) until the DMX
transport and shared RDM sequencing have been qualified.

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
- `stop`: release the UART and stop traffic.
- `status`: report active state, setup error, slot count and error counters.

The RX callback only signals frame availability. Reporting and snapshots run
in foreground context, not in the ISR.

## Prepared automatic hardware tests

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
an alternative for first bring-up. The S3 smoke path is prepared but untested
on hardware; `--tests all`/`rdm` are rejected before touching serial ports.

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
  changing a partly transmitted frame. Hardware-idle plus all queued bytes
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
5. Add shared RDM transaction sequencing and bounded turnaround/discovery.
6. Qualify RDM with real RS-485 transceivers, direction control and collisions.
