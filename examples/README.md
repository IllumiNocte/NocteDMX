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

The current package selects the ESP8266 backend, so the wiring comments name
its UART0 pins. When another backend is added, the application-facing calls
remain the same while pin/resource configuration moves into that backend.
Receive callbacks use `NOCTE_DMX_ISR_ATTR`, which maps to the interrupt-memory
attribute required by the selected backend.

The ESP8266 uses UART0 for DMX. Do not start `Serial` while an example is
using DMX or RDM. Always connect a suitable RS-485 transceiver; GPIO pins must
not be connected directly to a DMX line.

The `feather shield ESP8266_ESP32` directory is retained as a historical
hardware reference and is not a software example.
