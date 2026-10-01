#pragma once

// Keep platform selection in one place. A new controller family adds its
// backend below without leaking vendor headers into application code.
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)

#include "backends/esp8266/Esp8266DmxPort.h"

// Receive callbacks execute in the selected backend's interrupt context.
// Applications use this portable spelling instead of a vendor attribute.
#ifndef NOCTE_DMX_ISR_ATTR
#define NOCTE_DMX_ISR_ATTR IRAM_ATTR
#endif
namespace nocte {
namespace dmx {

namespace backends {
using Esp8266UartPort = ::LX8266DMX;
}

// Portable application-facing port type for the selected target.
using Port = backends::Esp8266UartPort;

// Existing ESP8266 applications historically use a single global port. Keep
// that object as the default while allowing portable code to avoid its legacy
// name.
inline Port& defaultPort() {
  return ::ESP8266DMX;
}

}  // namespace dmx
}  // namespace nocte

#else

#error "NocteDMX: no PHY backend is available for the selected architecture"
#endif
