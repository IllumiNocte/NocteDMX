#include "DmxFrame.h"

#include <string.h>

namespace nocte {
namespace dmx {
namespace core {

bool isValidOutputSlotCount(uint16_t slots) {
  return slots >= kMinimumOutputSlots && slots <= kMaximumSlots;
}

uint16_t clampOutputSlotCount(int slots) {
  if (slots < static_cast<int>(kMinimumOutputSlots)) {
    return kMinimumOutputSlots;
  }
  if (slots > static_cast<int>(kMaximumSlots)) {
    return kMaximumSlots;
  }
  return static_cast<uint16_t>(slots);
}

bool replaceChannelData(
    uint8_t* frame,
    const uint8_t* channels,
    uint16_t slots) {
  if (!frame || !channels || !isValidOutputSlotCount(slots)) {
    return false;
  }

  frame[0] = kNullStartCode;
  memcpy(frame + 1, channels, slots);
  return true;
}

uint16_t copyChannelData(
    const uint8_t* frame,
    uint16_t slots,
    uint8_t* destination,
    uint16_t capacity) {
  if (!frame || !destination || capacity == 0) {
    return 0;
  }

  const uint16_t copied = slots < capacity ? slots : capacity;
  memcpy(destination, frame + 1, copied);
  return copied;
}

}  // namespace core
}  // namespace dmx
}  // namespace nocte
