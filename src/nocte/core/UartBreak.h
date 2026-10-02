#pragma once
#include <stdint.h>

#if defined(__GNUC__)
#define NOCTE_UART_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define NOCTE_UART_INLINE __forceinline
#else
#define NOCTE_UART_INLINE inline
#endif

namespace nocte { namespace dmx { namespace core {

// GPIO IRQs can coalesce edges or add unequal latency to their timestamps.
// Once a response's leading BREAK was captured, do not flush valid UART bytes
// for a second GPIO-only "long low" without UART BREAK/framing corroboration.
// A corroborated short BREAK is still passed to the strict timing validator.
NOCTE_UART_INLINE bool qualifyUartLowPulse(
    uint32_t durationUs, uint32_t minimumUs, bool responseStarted, bool uartEvidence) {
  return durationUs >= minimumUs && (!responseStarted || uartEvidence);
}

} } }
#undef NOCTE_UART_INLINE
