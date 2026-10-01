# NocteDMX architecture

## Goals

- Keep DMX512-A and RDM behaviour independent of a controller vendor.
- Keep register, UART, PIO, DMA, interrupt, RTOS, and GPIO details inside a
  platform backend.
- Support multiple independent ports instead of assuming one global UART.
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
- `RdmPacket` builds common headers, decodes discovery replies, and validates
  observed controller responses including timing facts supplied by the PHY.

The existing `rdm` directory contains portable UID, table-of-devices, and
checksum utilities and will be folded into the same namespace incrementally.
Frame ownership, callback dispatch, and controller transaction state remain
the next extraction steps.

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

The backend reports received bytes, BREAK, idle timeout, transmission
completion, and hardware errors to the shared engine. It must not interpret
RDM PIDs or own application callbacks.

## Migration stages

1. Rename the package, introduce the stable facade, and isolate the existing
   ESP8266 implementation below `backends/esp8266`.
2. Extract shared constants, frame operations, packet construction and
   validation, then RDM state from the historical driver without changing
   ESP8266 behaviour. (In progress: constants/frame operations/RDM packet
   helpers are shared; ownership and transaction state still remain.)
3. Replace the remaining global-only assumptions with constructible ports.
4. Add the ESP32-S3 UART backend and validate it with the RP2040 HIL tester.
5. Add optional PIO/DMA or vendor-specific backends behind the same facade.

Every stage must keep the ESP8266 firmware compiling and retain the legacy
headers until a separately announced major-version removal.
