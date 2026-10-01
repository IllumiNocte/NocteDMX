#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Constants.h"

namespace nocte { namespace dmx { namespace core {

// Value type, independent of Arduino's String/Printable and vendor SDKs.
class Uid {
 public:
  Uid() : bytes_{} {}
  explicit Uid(uint64_t value) { setValue(value); }
  explicit Uid(const uint8_t* bytes) : bytes_{} {
    if (bytes) memcpy(bytes_, bytes, sizeof(bytes_));
  }

  const uint8_t* data() const { return bytes_; }
  uint8_t* data() { return bytes_; }
  uint64_t value() const {
    uint64_t result = 0;
    for (uint8_t i = 0; i < rdm::kUidSize; ++i) result = (result << 8) | bytes_[i];
    return result;
  }
  void setValue(uint64_t value) {
    for (uint8_t i = rdm::kUidSize; i > 0; --i) {
      bytes_[i - 1] = static_cast<uint8_t>(value);
      value >>= 8;
    }
  }
  bool operator==(const Uid& other) const {
    return memcmp(bytes_, other.bytes_, sizeof(bytes_)) == 0;
  }
  bool operator!=(const Uid& other) const { return !(*this == other); }
  bool operator<(const Uid& other) const {
    return memcmp(bytes_, other.bytes_, sizeof(bytes_)) < 0;
  }
  bool isBroadcast() const {
    // Manufacturer-specific broadcast UIDs also have an all-ones device ID.
    for (uint8_t i = 2; i < rdm::kUidSize; ++i) if (bytes_[i] != 0xFF) return false;
    return true;
  }
  bool setMidpoint(const Uid& a, const Uid& b) {
    const uint64_t low = a < b ? a.value() : b.value();
    const uint64_t high = a < b ? b.value() : a.value();
    if (high - low < 2) return false;
    setValue(low + (high - low) / 2);
    return true;
  }
  // Needs 14 bytes including NUL; failure leaves a valid empty string.
  bool format(char* destination, size_t capacity) const {
    if (!destination || capacity == 0) return false;
    destination[0] = '\0';
    if (capacity < 14) return false;
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (uint8_t i = 0; i < rdm::kUidSize; ++i) {
      if (i == 2) destination[out++] = ':';
      destination[out++] = hex[bytes_[i] >> 4];
      destination[out++] = hex[bytes_[i] & 0x0F];
    }
    destination[out] = '\0';
    return true;
  }

 private:
  uint8_t bytes_[rdm::kUidSize];
};

} } }
