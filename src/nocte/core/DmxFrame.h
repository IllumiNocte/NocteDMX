#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Constants.h"

namespace nocte {
namespace dmx {
namespace core {

// These operations intentionally contain no locking. A backend owns the
// critical section because interrupt, RTOS, and multi-core rules are platform
// specific.
bool isValidOutputSlotCount(uint16_t slots);
uint16_t clampOutputSlotCount(int slots);

bool replaceChannelData(
    uint8_t* frame,
    const uint8_t* channels,
    uint16_t slots);

uint16_t copyChannelData(
    const uint8_t* frame,
    uint16_t slots,
    uint8_t* destination,
    uint16_t capacity);

}  // namespace core
}  // namespace dmx
}  // namespace nocte
