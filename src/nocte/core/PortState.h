#pragma once

#include "Constants.h"

namespace nocte { namespace dmx { namespace core {

// One instance per port. Locking/ISR residency belong to the selected PHY.
// Updates are memory-atomic under the backend lock, not wire double-buffered.
struct FrameStorage {
  uint8_t dmx[kMaximumFrameSize] = {};
  uint8_t received[kMaximumFrameSize] = {};
  volatile uint16_t slots = kMaximumSlots;
  volatile uint16_t receivedLength = 0;
  uint16_t expectedLength = kMaximumFrameSize;
};

struct RdmTransactionState {
  uint8_t request[rdm::kMaximumFrameSize] = {};
  uint8_t response[rdm::kMaximumFrameSize] = {};
  uint8_t discovery[rdm::kDiscoveryDiagnosticBytes] = {};
  uint8_t discoveryLength = 0;
  uint8_t transaction = 0;
  uint16_t transmitLength = 0;
  uint16_t responseLength = 0;
  uint16_t validationFailures = 0;
  volatile uint8_t handled = 0;
  volatile uint8_t breakSeen = 0;
  volatile uint32_t lastSlotUs = 0;
  volatile uint32_t maximumSlotIntervalUs = 0;
  volatile uint32_t requestEndUs = 0;
  volatile uint32_t firstSlotDelayUs = 0;
};

} } }
