#pragma once

#include <stdint.h>

namespace nocte {
namespace dmx {

// Protocol limits shared by every PHY backend. A DMX frame contains one
// start code followed by up to 512 data slots.
static constexpr uint16_t kMinimumOutputSlots = 24;
static constexpr uint16_t kMinimumReceiveSlots = 1;
static constexpr uint16_t kMaximumSlots = 512;
static constexpr uint16_t kMaximumFrameSize = kMaximumSlots + 1;
static constexpr uint8_t kNullStartCode = 0x00;

// Public start mode for a bidirectional port. Transient backend states used
// while switching between DMX and RDM deliberately stay backend-private.
enum class PortMode : uint8_t {
  Receive = 0,
  Send = 1,
};

namespace rdm {

static constexpr uint16_t kMaximumFrameSize = 257;
static constexpr uint8_t kDiscoveryDiagnosticBytes = 32;
static constexpr uint8_t kUidSize = 6;
static constexpr uint8_t kMinimumMessageLength = 24;
static constexpr uint8_t kChecksumSize = 2;
static constexpr uint8_t kStartCode = 0xCC;
static constexpr uint8_t kSubStartCode = 0x01;
static constexpr uint8_t kMaximumParameterDataLength = 231;

}  // namespace rdm
}  // namespace dmx
}  // namespace nocte
