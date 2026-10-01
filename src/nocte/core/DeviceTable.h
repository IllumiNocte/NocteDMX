#pragma once

#include "Uid.h"

namespace nocte { namespace dmx { namespace core {

// Fixed capacity. Indices are device indices, never raw byte offsets.
template <size_t Capacity>
class DeviceTable {
 public:
  static_assert(Capacity > 0, "A device table needs at least one entry");
  DeviceTable() : storage_{}, count_(0) {}
  size_t count() const { return count_; }
  size_t capacity() const { return Capacity; }
  void clear() { count_ = 0; memset(storage_, 0, sizeof(storage_)); }
  bool append(const Uid& uid) {
    if (count_ == Capacity) return false;
    memcpy(storage_ + count_++ * rdm::kUidSize, uid.data(), rdm::kUidSize);
    return true;
  }
  bool add(const Uid& uid) { return !contains(uid) && append(uid); }
  bool get(size_t index, Uid& uid) const {
    if (index >= count_) return false;
    uid = Uid(storage_ + index * rdm::kUidSize);
    return true;
  }
  bool contains(const Uid& uid) const {
    for (size_t i = 0; i < count_; ++i) {
      if (memcmp(storage_ + i * rdm::kUidSize, uid.data(), rdm::kUidSize) == 0) return true;
    }
    return false;
  }
  bool remove(size_t index) {
    if (index >= count_) return false;
    --count_;
    memmove(storage_ + index * rdm::kUidSize,
            storage_ + (index + 1) * rdm::kUidSize,
            (count_ - index) * rdm::kUidSize);
    memset(storage_ + count_ * rdm::kUidSize, 0, rdm::kUidSize);
    return true;
  }
  bool pop(Uid& uid) {
    if (count_ == 0) return false;
    get(count_ - 1, uid);
    return remove(count_ - 1);
  }
  uint8_t* data() { return storage_; }
  const uint8_t* data() const { return storage_; }

 private:
  uint8_t storage_[Capacity * rdm::kUidSize];
  size_t count_;
};

} } }
