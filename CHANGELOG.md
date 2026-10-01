# Changelog

## 0.1.0 - Unreleased

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
