#pragma once

#include "PortState.h"

// Header-only so a PHY ISR can inline these bounded operations into IRAM
// without introducing Arduino/vendor headers in the portable core.
#if defined(__GNUC__)
#define NOCTE_CORE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define NOCTE_CORE_INLINE __forceinline
#else
#define NOCTE_CORE_INLINE inline
#endif

namespace nocte { namespace dmx { namespace core {

class DmxReceiver {
 public:
  explicit DmxReceiver(FrameStorage& frames) : frames_(frames) {}

  NOCTE_CORE_INLINE void reset() {
    receiving_ = false;
    frames_.receivedLength = 0;
  }

  // Publish only at the next qualified BREAK. This preserves valid long
  // inter-slot pauses and rejects >512-slot frames without publishing a prefix.
  // 0 means no valid frame was completed; otherwise returns channel count.
  NOCTE_CORE_INLINE uint16_t onBreak() {
    uint16_t completed = 0;
    const uint16_t length = frames_.receivedLength;
    if (receiving_ && length >= kMinimumReceiveSlots + 1
        && length <= kMaximumFrameSize) {
      for (uint16_t i = 0; i < length; ++i) frames_.dmx[i] = frames_.received[i];
      for (uint16_t i = length; i < kMaximumFrameSize; ++i) frames_.dmx[i] = 0;
      completed = length - 1;
      frames_.slots = completed;
    }
    frames_.receivedLength = 0;
    receiving_ = true;
    return completed;
  }

  NOCTE_CORE_INLINE void onByte(uint8_t byte) {
    if (!receiving_) return;
    const uint16_t length = frames_.receivedLength;
    if (length >= kMaximumFrameSize || (length == 0 && byte != kNullStartCode)) {
      reset(); // Wait for the next BREAK; never expose partial/unknown frames.
      return;
    }
    frames_.received[length] = byte;
    frames_.receivedLength = length + 1;
  }

  NOCTE_CORE_INLINE void onError() { reset(); }

 private:
  FrameStorage& frames_;
  bool receiving_ = false;
};

} } }

#undef NOCTE_CORE_INLINE
