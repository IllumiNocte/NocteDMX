#pragma once

#include "RdmPacket.h"

#if defined(__GNUC__)
#define NOCTE_RDM_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define NOCTE_RDM_INLINE __forceinline
#else
#define NOCTE_RDM_INLINE inline
#endif

namespace nocte { namespace dmx { namespace core {

struct RdmReceiveTiming {
  uint32_t breakUs, mabUs, responseSpacingUs, maximumSlotIntervalUs, packetUs;
};

// Bounded normal (non-discovery) controller response capture. The PHY supplies
// wire timestamps and serializes calls. No vendor headers, heap, or ISR callbacks.
// E1.20-2025 tables 3-1/3-2: receiver BREAK 88..352, MAB 8..88,
// EOP->SOP 176..2800, missing response 3000, byte interval <=44+2100 us.
class RdmReceiver {
 public:
  NOCTE_RDM_INLINE void begin(uint32_t requestEndUs) {
    requestEnd_ = requestEndUs;
    length_ = failures_ = 0;
    breakSeen_ = firstStartSeen_ = closed_ = lowActive_ = false;
    breakStart_ = breakEnd_ = lastByte_ = maximumInterval_ = mabUs_ = 0;
  }
  NOCTE_RDM_INLINE void onLow(uint32_t nowUs) { lowActive_ = true; lowStart_ = nowUs; }
  NOCTE_RDM_INLINE void onHigh() { lowActive_ = false; }
  NOCTE_RDM_INLINE void onBreak(uint32_t startUs, uint32_t endUs) {
    if (closed_) return;
    if (breakSeen_ || length_) failures_ |= kRdmUnexpectedResponse;
    breakSeen_ = true;
    breakStart_ = startUs;
    breakEnd_ = endUs;
    const uint32_t spacing = startUs - requestEnd_;
    if (spacing < 176) failures_ |= kRdmResponseTooEarly;
    if (spacing > 2800) failures_ |= kRdmResponseTooLate;
    if (endUs - startUs < 88 || endUs - startUs > 352)
      failures_ |= kRdmInvalidPhysicalTiming;
  }
  NOCTE_RDM_INLINE void onStartBit(uint32_t nowUs) {
    if (!closed_ && breakSeen_ && !firstStartSeen_) {
      firstStartSeen_ = true;
      const uint32_t mab = nowUs - breakEnd_;
      mabUs_ = mab;
      if (mab < 8 || mab > 88) failures_ |= kRdmInvalidPhysicalTiming;
    }
  }
  NOCTE_RDM_INLINE void onByte(uint8_t value, uint32_t endUs) {
    if (closed_) return;
    if (length_) {
      const uint32_t interval = endUs - lastByte_;
      if (interval > maximumInterval_) maximumInterval_ = interval;
      if (interval > 2144) failures_ |= kRdmInterSlotTimeout;
    }
    lastByte_ = endUs;
    if (length_ < rdm::kMaximumFrameSize) bytes_[length_++] = value;
    else failures_ |= kRdmLengthMismatch; // Never write outside the fixed buffer.
    if (breakSeen_ && length_ >= 3 && bytes_[2] >= rdm::kMinimumMessageLength) {
      const uint32_t wireLength = static_cast<uint32_t>(bytes_[2]) + 2;
      const uint32_t maximumPacketTime = 440 + wireLength * 44 + (wireLength - 1) * 76;
      if (endUs - breakStart_ > maximumPacketTime) failures_ |= kRdmPacketTimeExceeded;
    }
  }
  NOCTE_RDM_INLINE void onError() { failures_ |= kRdmReceiveError; }
  NOCTE_RDM_INLINE bool poll(uint32_t nowUs) {
    if (closed_) return true;
    // Do not finish at the declared length: an additional byte within the legal
    // inter-slot window must invalidate the packet, not become the next packet.
    if ((length_ && nowUs - lastByte_ > 2144)
        || (!breakSeen_ && !length_ && nowUs - requestEnd_ >= 3000
            && (!lowActive_ || nowUs - lowStart_ > 352))
        || nowUs - requestEnd_ >= 40000) {
      // 2800-us SOP + 31204-us maximum packet + 2145-us quiet closure.
      if (nowUs - requestEnd_ >= 40000) failures_ |= kRdmFrameNotClosed;
      closed_ = true;
    }
    return closed_;
  }
  uint16_t validate(const uint8_t* request, uint16_t requestLength) const {
    if (!length_ && !breakSeen_ && !failures_) return 0; // A normal timeout.
    const RdmResponseObservation observation = {
      breakSeen_, closed_, 0, maximumInterval_, 0, 2144};
    uint16_t failures = failures_ | validateRdmResponse(bytes_, length_, observation);
    if (breakSeen_ && length_ && !firstStartSeen_) failures |= kRdmInvalidPhysicalTiming;
    if (!failures && !matchesRdmResponse(request, requestLength, bytes_, length_))
      failures |= kRdmUnexpectedResponse;
    return failures;
  }
  const uint8_t* data() const { return bytes_; }
  uint16_t length() const { return length_; }
  uint32_t lastByteUs() const { return lastByte_; }
  RdmReceiveTiming timing() const {
    return {breakSeen_ ? breakEnd_ - breakStart_ : 0, mabUs_,
      breakSeen_ ? breakStart_ - requestEnd_ : 0, maximumInterval_,
      breakSeen_ && length_ ? lastByte_ - breakStart_ : 0};
  }
 private:
  uint8_t bytes_[rdm::kMaximumFrameSize] = {};
  uint16_t length_ = 0, failures_ = 0;
  uint32_t requestEnd_ = 0, breakStart_ = 0, breakEnd_ = 0;
  uint32_t lastByte_ = 0, maximumInterval_ = 0, mabUs_ = 0, lowStart_ = 0;
  bool breakSeen_ = false, firstStartSeen_ = false, closed_ = false, lowActive_ = false;
};

} } }
#undef NOCTE_RDM_INLINE
