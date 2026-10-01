# Changelog

## 0.1.0 - Unreleased

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

This release retains the DMX/RDM behaviour of the preceding uNode-maintained
LXESP8266DMX fork while establishing the structure for additional backends.
