# NocteDMX architecture

## Goals

- Keep DMX512-A and RDM behaviour independent of a controller vendor.
- Keep register, UART, PIO, DMA, interrupt, RTOS, and GPIO details inside a
  platform backend.
- Support independent port objects; a backend must enforce hardware limits.
- Avoid heap allocation and Arduino-only types in the future shared core.
- Preserve the existing ESP8266 API while applications migrate.

## Layers

### Public facade

`NocteDMX.h` is the only header new applications include. It exposes the
selected `nocte::dmx::Port` and, while the ESP8266 migration is in progress,
`nocte::dmx::defaultPort()`.

### Platform selection

`nocte/NocteDmxPort.h` selects one backend at compile time. Adding a new
controller must not require changes in application source files.

### Portable core

`nocte/core` contains code which does not include Arduino or vendor headers:

- `Constants.h` owns protocol limits shared by all backends.
- `DmxFrame` validates slot counts and copies channel data while the backend
  remains responsible for the appropriate critical section.
- `DmxReceiver` assembles null-start-code DMX frames from qualified BREAK and
  byte events. It discards unknown start codes, oversized and damaged frames,
  and preserves the last valid snapshot. It publishes at the next BREAK,
  not an arbitrary inter-slot idle timeout. It currently serves the S3 backend.
- `RdmPacket` builds common headers, decodes discovery replies, and validates
  observed controller responses including timing facts supplied by the PHY.
- `RdmReceiver` captures a normal controller response from BREAK, first START
  bit, byte timestamps and hardware-error events. It has a fixed 257-byte
  buffer, bounded deadlines and wrap-safe timing checks. The S3 uses this
  adapter; ESP8266 receive/discovery scheduling has not yet migrated to it.
- `Uid` stores, compares, formats, and bisects six-byte identifiers without
  Arduino `String`, `Printable`, or heap allocation.
- `DeviceTable<Capacity>` owns a bounded list of unique identifiers.
- `PortState` owns the DMX buffers and RDM transaction buffers/observations,
  separately for each port instance.

The historical `rdm/UID.h` and `rdm/TOD.h` adapt the portable types to the old
Arduino API. Checksum utilities retain their compatibility entry points.
The ESP8266 scheduler still owns receive assembly, callback dispatch, and
transaction sequencing. Shared storage is not yet a complete shared engine.

### PHY backends

A backend owns all controller-specific details:

- UART, PIO, timer, and DMA resource selection
- TX/RX pin routing
- DE and active-low `/RE` control
- BREAK and Mark-After-Break generation/detection
- end-of-wire detection rather than FIFO-empty guesses
- receive-idle and framing-error events
- interrupt-safe monotonic timestamps
- critical sections required by its execution model

The engine/PHY boundary is received bytes, BREAK, first START bit, idle timeout,
transmission completion, and hardware errors. The S3 now supplies normal RDM
response events; the ESP8266 scheduler still handles its receive events directly.
Application-specific PIDs must remain outside the PHY.

## Current port contract

- A port is non-copyable and owns its frame, transaction, and callback state.
- Start/stop, configuration, and blocking RDM commands are foreground-only;
  the current API is not thread-safe or reentrant.
- ESP8266 UART0 has one owner. A second instance cannot start until the owner
  stops; check `isActive()` after starting. Destroying an inactive port must
  not stop the owner. This does not provide two physical ESP8266 ports.
- ESP8266 `setFrame`/`copyFrame` preserve the previous interrupt mask. They provide a
  memory-consistent update/snapshot, not wire-frame double buffering.
- Receive callbacks run in interrupt context. Set a flag and return; do not
  allocate, block, print, or start controller transactions from a callback.
- `setUid(core::Uid(...))` configures the instance's source UID. Until called,
  the legacy static `THIS_DEVICE_ID` is used for compatibility.
- Typed GET/SET report ACK, NACK, ACK_TIMER, ACK_OVERFLOW, timeout, invalid
  response, or invalid arguments. GET copies at most `min(capacity, PDL)` and
  reports both lengths. Deferred/overflow results are not automatically
  retried or assembled; broadcast SET is not supported by these helpers.
- Responses must match both UIDs, transaction, PID, command class, and
  sub-device, as well as their declared length, PDL, checksum, and measured
  response timing. Diagnostics describe the most recent transaction.
  GET:QUEUED_MESSAGE is the explicit PID-correlation exception: an ACK can
  carry the queued PID or STATUS_MESSAGES (E1.20-2025 section 10.3.1).

The experimental ESP32-S3 backend owns UART1/2 and its GPIOs exclusively between
NocteDMX instances. It rejects a UART already occupied by the IDF driver;
applications must not start another UART driver on that resource afterwards.
Spinlocks protect snapshots across cores; an immutable per-frame TX buffer
prevents mid-frame updates on the wire. Completion requires all bytes queued
and the hardware shift register idle. An output task is allocated at start;
the steady-state data path does not allocate. RX assembly uses `DmxReceiver`.
RDM GET/SET pauses the same output task at a complete DMX frame boundary, runs
one bounded half-duplex transaction, and resumes output without task allocation.
The caller waits for the previous pause acknowledgement to clear before another
command can begin. TX completion busy-polls the actual UART FSM before releasing
DE; direct UART functional tests do not qualify physical DE//RE timing.
See [the S3 guide](esp32s3.md) for restrictions and pending hardware validation.

Next, add discovery/Mute/Unmute and migrate more sequencing against this real
second backend without duplicating the ESP8266 register scheduler or inventing
unused virtual interfaces. Native core tests and the [standalone HIL suite](../tests/hil/README.md)
remain the regression boundary; precise timing qualification is a separate
hardware exercise. S3 UART1 DMX has passed direct-UART smoke tests;
S3 typed GET/SET and malformed-response recovery have direct-UART coverage;
full qualification, UART2, S3 discovery and responder operation remain pending.

## Migration stages

1. Rename the package, introduce the stable facade, and isolate the existing
   ESP8266 implementation below `backends/esp8266`.
2. Extract shared constants, frame operations, packet construction and
   validation, then RDM state from the historical driver without changing
   ESP8266 behaviour. (In progress: constants/frame operations/RDM packet
   helpers, UID/table utilities, frame storage and transaction storage are
   shared; receive assembly and transaction sequencing still remain.)
3. Replace global-only assumptions with constructible ports. (Implemented
   for ESP8266 with exclusive UART0 ownership and per-instance UID support.)
4. Add the ESP32-S3 UART backend and validate it with the RP2040 HIL tester.
   (Experimental DMX backend added and UART1 direct-UART smoke tests passed;
   shared normal RDM capture and controller GET/SET added; discovery, responder
   operation and full qualification still pending.)
5. Add optional PIO/DMA or vendor-specific backends behind the same facade.

Every stage must keep the ESP8266 firmware compiling and retain the legacy
headers until a separately announced major-version removal.
