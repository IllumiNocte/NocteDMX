#pragma once

#include "DeviceTable.h"
#include "RdmPacket.h"

namespace nocte { namespace dmx { namespace core {

enum class RdmScanStatus : uint8_t {
  Complete, CapacityExceeded, TransactionLimit, Unresolved, TransportError, InvalidArgument,
};
struct RdmScanResult {
  RdmScanStatus status;
  uint16_t transactions;
  uint16_t unresolved;
};

// Foreground, bounded full discovery. Transport supplies discoverRdmBranch and
// setRdmDiscoveryMute. Fixed 49-entry DFS stack, no recursion or heap. Devices
// remain muted after the scan; ordinary GET/SET and DMX are unaffected.
template <typename Transport, size_t Capacity>
RdmScanResult scanRdmDevices(Transport& transport, DeviceTable<Capacity>& devices,
                            uint16_t transactionLimit = 512) {
  if (transactionLimit < 2) return {RdmScanStatus::InvalidArgument, 0, 0};
  devices.clear();
  RdmScanResult result = {RdmScanStatus::Complete, 1, 0};
  const Uid all(UINT64_C(0xFFFFFFFFFFFF));
  if (transport.setRdmDiscoveryMute(all, false).status != RdmCommandStatus::Sent)
    return {RdmScanStatus::TransportError, 1, 0};
  struct Range { Uid low, high; };
  Range stack[49];
  size_t count = 1;
  stack[0] = {Uid(UINT64_C(0)), all};
  while (count) {
    const Range range = stack[--count];
    if (result.transactions == transactionLimit) {
      result.status = RdmScanStatus::TransactionLimit;
      return result;
    }
    ++result.transactions;
    Uid found;
    const auto reply = transport.discoverRdmBranch(range.low, range.high, &found);
    if (reply == RdmDiscoveryResult::None) continue;
    const bool leaf = range.low == range.high;
    const bool candidate = reply == RdmDiscoveryResult::SingleDevice
        && !(found < range.low) && !(range.high < found) && !found.isBroadcast();
    // At a leaf, try its exact UID even after malformed collision data (7.3).
    if (candidate || leaf) {
      if (leaf) found = range.low;
      if (result.transactions == transactionLimit) {
        result.status = RdmScanStatus::TransactionLimit;
        return result;
      }
      ++result.transactions;
      const auto muted = transport.setRdmDiscoveryMute(found, true);
      if (muted.ok() && !devices.contains(found)) {
        if (!devices.add(found)) {
          result.status = RdmScanStatus::CapacityExceeded;
          return result;
        }
        if (!leaf) stack[count++] = range; // Search SAME branch after muting.
        continue;
      }
    }
    if (leaf) { ++result.unresolved; continue; }
    const uint64_t middle = range.low.value() + (range.high.value() - range.low.value()) / 2;
    stack[count++] = {Uid(middle + 1), range.high};
    stack[count++] = {range.low, Uid(middle)};
  }
  if (result.unresolved) result.status = RdmScanStatus::Unresolved;
  return result;
}

} } }
