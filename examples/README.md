# NocteDMX examples

The examples are intentionally small and use the public `<NocteDMX.h>`
facade. They do not include a backend header or refer to the historical
`LX8266DMX` class and `ESP8266DMX` global.

| Example | Purpose | Additional dependency |
| --- | --- | --- |
| `DmxOutputFade` | Atomic DMX output frame updates | none |
| `DmxInput` | Interrupt-safe DMX input and frame copy | none |
| `DmxNeoPixelInput` | DMX-to-RGB pixel mapping | Adafruit NeoPixel |
| `RdmControllerDiscovery` | RDM discovery, GET, SET, and identify | none |
| `RdmResponder` | Minimal discoverable RDM responder | none |

The package selects the ESP8266 or experimental ESP32-S3 backend. The three
DMX examples compile on both; the RDM examples currently require ESP8266.
Wiring comments describe ESP8266 UART0. On S3 the defaults are UART1, TX GPIO17,
RX GPIO18. The direction pin defaults to GPIO5 in the shared examples; for
direct UART testing define `NOCTE_HIL_DIRECTION_PIN=255` when building.
The dedicated [S3 bench sketch](../extras/hil/Esp32S3UartBench/Esp32S3UartBench.ino)
needs no LED or pixel hardware and has direction control disabled by default.
See [the S3 guide](../docs/esp32s3.md) for wiring and its experimental status.
Receive callbacks use `NOCTE_DMX_ISR_ATTR`, which maps to the interrupt-memory
attribute required by the selected backend.

The ESP8266 uses UART0 for DMX. Do not start `Serial` while an example is
using DMX or RDM. Always connect a suitable RS-485 transceiver; GPIO pins must
not be connected directly to a DMX line.
On S3 the USB CDC console is separate from DMX UART1. Direct UART bench wiring
is allowed only between 3.3-V logic devices sharing GND, never to RS-485 A/B.

The `feather shield ESP8266_ESP32` directory is retained as a historical
hardware reference and is not a software example.
