#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#include <nocte/core/Constants.h>
#include <nocte/core/DmxFrame.h>
#include <nocte/core/RdmPacket.h>
#include <nocte/core/Uid.h>
#include <nocte/core/DeviceTable.h>
#include <nocte/core/PortState.h>
#include <nocte/core/DmxReceiver.h>
#include <nocte/core/RdmReceiver.h>
#include <nocte/core/UartBreak.h>
#include <nocte/core/RdmDiscovery.h>
#include <rdm/rdm_utility.h>

namespace {

int failures = 0;

#define EXPECT_TRUE(expression)                                              \
  do {                                                                       \
    if (!(expression)) {                                                     \
      std::cerr << __FILE__ << ':' << __LINE__                              \
                << ": expectation failed: " #expression << '\n';           \
      ++failures;                                                            \
    }                                                                        \
  } while (false)

#define EXPECT_EQ(actual, expected) EXPECT_TRUE((actual) == (expected))

using nocte::dmx::core::RdmDiscoveryResult;
using nocte::dmx::core::RdmResponseObservation;

void appendRdmChecksum(uint8_t* packet) {
  const uint8_t messageLength = packet[RDM_IDX_PACKET_SIZE];
  const uint16_t checksum = rdmChecksum(packet, messageLength);
  packet[messageLength] = static_cast<uint8_t>(checksum >> 8);
  packet[messageLength + 1] = static_cast<uint8_t>(checksum & 0xFF);
}

void testDmxFrameLimits() {
  using namespace nocte::dmx;

  EXPECT_TRUE(!core::isValidOutputSlotCount(kMinimumOutputSlots - 1));
  EXPECT_TRUE(core::isValidOutputSlotCount(kMinimumOutputSlots));
  EXPECT_TRUE(core::isValidOutputSlotCount(kMaximumSlots));
  EXPECT_TRUE(!core::isValidOutputSlotCount(kMaximumSlots + 1));

  EXPECT_EQ(core::clampOutputSlotCount(-1), kMinimumOutputSlots);
  EXPECT_EQ(core::clampOutputSlotCount(kMinimumOutputSlots),
            kMinimumOutputSlots);
  EXPECT_EQ(core::clampOutputSlotCount(100), 100);
  EXPECT_EQ(core::clampOutputSlotCount(kMaximumSlots + 1), kMaximumSlots);
}

void testDmxFrameCopying() {
  using namespace nocte::dmx;

  uint8_t channels[kMinimumOutputSlots];
  uint8_t frame[kMinimumOutputSlots + 1];
  for (uint16_t index = 0; index < kMinimumOutputSlots; ++index) {
    channels[index] = static_cast<uint8_t>(index + 1);
  }
  std::fill(frame, frame + sizeof(frame), static_cast<uint8_t>(0xFF));

  EXPECT_TRUE(core::replaceChannelData(
      frame, channels, kMinimumOutputSlots));
  EXPECT_EQ(frame[0], kNullStartCode);
  EXPECT_TRUE(std::memcmp(frame + 1, channels, sizeof(channels)) == 0);
  EXPECT_TRUE(!core::replaceChannelData(
      nullptr, channels, kMinimumOutputSlots));
  EXPECT_TRUE(!core::replaceChannelData(
      frame, nullptr, kMinimumOutputSlots));
  EXPECT_TRUE(!core::replaceChannelData(
      frame, channels, kMinimumOutputSlots - 1));

  uint8_t copied[4] = {};
  EXPECT_EQ(core::copyChannelData(
                frame, kMinimumOutputSlots, copied, sizeof(copied)),
            sizeof(copied));
  EXPECT_TRUE(std::memcmp(copied, channels, sizeof(copied)) == 0);
  EXPECT_EQ(core::copyChannelData(frame, 4, copied, 0), 0);
  EXPECT_EQ(core::copyChannelData(nullptr, 4, copied, sizeof(copied)), 0);
}

void testRdmPacketConstruction() {
  using namespace nocte::dmx;

  const uint8_t sourceUid[rdm::kUidSize] = {
      0x7A, 0x70, 0x01, 0x02, 0x03, 0x04};
  uint8_t packet[RDM_PKT_BASE_TOTAL_LEN] = {};

  core::initializeRdmControllerHeader(
      packet, RDM_PKT_BASE_MSG_LEN, sourceUid, 0x42, RDM_PORT_ONE, 0x1234);
  core::setRdmParameterHeader(packet, RDM_GET_COMMAND, RDM_DEVICE_INFO, 0);

  EXPECT_EQ(packet[RDM_IDX_START_CODE], rdm::kStartCode);
  EXPECT_EQ(packet[RDM_IDX_SUB_START_CODE], rdm::kSubStartCode);
  EXPECT_EQ(packet[RDM_IDX_PACKET_SIZE], RDM_PKT_BASE_MSG_LEN);
  EXPECT_TRUE(std::memcmp(
      packet + RDM_IDX_SOURCE_UID, sourceUid, rdm::kUidSize) == 0);
  EXPECT_EQ(packet[RDM_IDX_TRANSACTION_NUM], 0x42);
  EXPECT_EQ(packet[RDM_IDX_PORT], RDM_PORT_ONE);
  EXPECT_EQ(packet[RDM_IDX_MSG_COUNT], 0);
  EXPECT_EQ(packet[RDM_IDX_SUB_DEV_MSB], 0x12);
  EXPECT_EQ(packet[RDM_IDX_SUB_DEV_LSB], 0x34);
  EXPECT_EQ(packet[RDM_IDX_CMD_CLASS], RDM_GET_COMMAND);
  EXPECT_EQ(packet[RDM_IDX_PID_MSB], 0x00);
  EXPECT_EQ(packet[RDM_IDX_PID_LSB], 0x60);
  EXPECT_EQ(packet[RDM_IDX_PARAM_DATA_LEN], 0);
  EXPECT_EQ(core::rdmWireLength(RDM_PKT_BASE_MSG_LEN),
            RDM_PKT_BASE_TOTAL_LEN);

  std::fill(packet, packet + sizeof(packet), static_cast<uint8_t>(0));
  core::initializeRdmResponderHeader(
      packet,
      RDM_PKT_BASE_MSG_LEN,
      sourceUid,
      0x11,
      RDM_RESPONSE_TYPE_ACK,
      3,
      RDM_ROOT_DEVICE);
  EXPECT_EQ(packet[RDM_IDX_RESPONSE_TYPE], RDM_RESPONSE_TYPE_ACK);
  EXPECT_EQ(packet[RDM_IDX_MSG_COUNT], 3);
}

void testRdmResponseValidation() {
  using namespace nocte::dmx;

  uint8_t packet[RDM_PKT_BASE_TOTAL_LEN] = {};
  packet[RDM_IDX_START_CODE] = rdm::kStartCode;
  packet[RDM_IDX_SUB_START_CODE] = rdm::kSubStartCode;
  packet[RDM_IDX_PACKET_SIZE] = RDM_PKT_BASE_MSG_LEN;
  appendRdmChecksum(packet);

  RdmResponseObservation observation = {
      true,
      true,
      316,
      100,
      316,
      2144,
  };
  EXPECT_EQ(core::validateRdmResponse(
                packet, sizeof(packet), observation),
            core::kRdmResponseValid);

  packet[RDM_PKT_BASE_MSG_LEN + 1] ^= 0x01;
  EXPECT_EQ(core::validateRdmResponse(
                packet, sizeof(packet), observation),
            core::kRdmChecksumMismatch);
  packet[RDM_PKT_BASE_MSG_LEN + 1] ^= 0x01;

  observation.breakSeen = false;
  observation.frameClosed = false;
  observation.firstSlotDelayUs = 315;
  observation.maximumSlotIntervalUs = 2145;
  const uint16_t timingFailures = core::validateRdmResponse(
      packet, sizeof(packet), observation);
  EXPECT_TRUE((timingFailures & core::kRdmMissingBreak) != 0);
  EXPECT_TRUE((timingFailures & core::kRdmFrameNotClosed) != 0);
  EXPECT_TRUE((timingFailures & core::kRdmResponseTooEarly) != 0);
  EXPECT_TRUE((timingFailures & core::kRdmInterSlotTimeout) != 0);

  observation = {true, true, 316, 100, 316, 2144};
  packet[RDM_IDX_START_CODE] = 0;
  EXPECT_TRUE((core::validateRdmResponse(
      packet, sizeof(packet), observation) & core::kRdmInvalidStartCode) != 0);
  EXPECT_TRUE((core::validateRdmResponse(
      nullptr, 0, observation) & core::kRdmFrameTooShort) != 0);
}

void encodeDiscoveryByte(uint8_t value, uint8_t* encoded) {
  encoded[0] = static_cast<uint8_t>(value | 0xAA);
  encoded[1] = static_cast<uint8_t>(value | 0x55);
}

void testRdmDiscoveryDecoding() {
  using namespace nocte::dmx;

  uint8_t response[24] = {};
  std::fill(
      response,
      response + 7,
      static_cast<uint8_t>(RDM_DISC_PREAMBLE));
  response[7] = RDM_DISC_PREAMBLE_SEPARATOR;

  const uint8_t uid[rdm::kUidSize] = {
      0x7A, 0x70, 0x10, 0x20, 0x30, 0x40};
  uint8_t* encoded = response + 8;
  for (uint8_t index = 0; index < rdm::kUidSize; ++index) {
    encodeDiscoveryByte(uid[index], encoded + index * 2);
  }
  const uint16_t checksum = rdmChecksum(encoded, 12);
  encodeDiscoveryByte(static_cast<uint8_t>(checksum >> 8), encoded + 12);
  encodeDiscoveryByte(static_cast<uint8_t>(checksum & 0xFF), encoded + 14);

  uint8_t decoded[rdm::kUidSize] = {};
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(
                response, sizeof(response), decoded),
            RdmDiscoveryResult::SingleDevice);
  EXPECT_TRUE(std::memcmp(decoded, uid, sizeof(uid)) == 0);
  for (uint16_t removed = 0; removed <= 7; ++removed)
    EXPECT_EQ(core::decodeRdmDiscoveryResponse(response + removed, 24 - removed, decoded),
              RdmDiscoveryResult::SingleDevice);
  response[0] = 0xFD;
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(response, 24, decoded),
            RdmDiscoveryResult::CollisionOrMalformed);
  response[0] = 0xFE;
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(nullptr, 0, decoded),
            RdmDiscoveryResult::None);

  // Corrupt an encoded UID byte. Changing only a masked padding bit would
  // leave the decoded UID unchanged and is therefore not a useful vector.
  response[8] = 0;
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(
                response, sizeof(response), decoded),
            RdmDiscoveryResult::CollisionOrMalformed);
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(
                response, sizeof(response) - 1, decoded),
            RdmDiscoveryResult::CollisionOrMalformed);
}

void testDiscoveryReceiverAndScan() {
  using namespace nocte::dmx::core;
  uint8_t wire[24];
  std::fill(wire, wire + 7, static_cast<uint8_t>(0xFE));
  wire[7] = 0xAA;
  const Uid target(UINT64_C(0x7FF052444D01));
  for (unsigned i = 0; i < 6; ++i) encodeDiscoveryByte(target.data()[i], wire + 8 + 2 * i);
  const auto checksum = rdmChecksum(wire + 8, 12);
  encodeDiscoveryByte(static_cast<uint8_t>(checksum >> 8), wire + 20);
  encodeDiscoveryByte(static_cast<uint8_t>(checksum), wire + 22);
  RdmDiscoveryReceiver receiver;
  uint8_t decoded[6] = {};
  for (unsigned removed = 0; removed <= 7; ++removed) {
    for (uint32_t spacing : {176u, 2800u}) {
      receiver.begin(100);
      receiver.onActivity(100 + spacing);
      for (unsigned i = removed; i < 24; ++i)
        receiver.onByte(wire[i], 100 + spacing + (i - removed + 1) * 44);
      EXPECT_TRUE(!receiver.poll(5899));
      EXPECT_TRUE(receiver.poll(5900));
      EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::SingleDevice);
      EXPECT_EQ(Uid(decoded), target);
    }
  }
  // GPIO/UART ISR timestamps are authoritative even if the foreground task
  // is not scheduled until several milliseconds after the reply completes.
  receiver.begin(100); receiver.onActivity(600);
  for (unsigned i = 0; i < 24; ++i) receiver.onByte(wire[i], 600 + (i + 1) * 44);
  EXPECT_TRUE(receiver.poll(9100));
  EXPECT_EQ(receiver.timing().responseSpacingUs, 500);
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::SingleDevice);
  receiver.begin(100);
  EXPECT_TRUE(receiver.poll(5900));
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::None);
  receiver.begin(100); receiver.onActivity(600); receiver.poll(5900);
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::CollisionOrMalformed);
  for (uint32_t spacing : {175u, 2801u}) {
    receiver.begin(100); receiver.onActivity(100 + spacing);
    for (unsigned i = 0; i < 24; ++i) receiver.onByte(wire[i], 100 + spacing + (i + 1) * 44);
    receiver.poll(5900);
    EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::CollisionOrMalformed);
  }
  receiver.begin(100); receiver.onActivity(600); receiver.onError(); receiver.poll(5900);
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::CollisionOrMalformed);
  // Latest legal SOP + longest legal discovery packet: 5800-us request gap
  // alone is not enough; preserve 176 us after the actual final response slot.
  receiver.begin(100); receiver.onActivity(2900);
  for (unsigned i = 0; i < 23; ++i) receiver.onByte(wire[i], 2900 + (i + 1) * 44);
  receiver.onByte(wire[23], 5800);
  EXPECT_TRUE(!receiver.poll(5900));
  EXPECT_TRUE(!receiver.poll(5975));
  EXPECT_TRUE(receiver.poll(5976));
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::SingleDevice);
  receiver.begin(100); receiver.onActivity(2900);
  for (unsigned i = 0; i < 23; ++i) receiver.onByte(wire[i], 2900 + (i + 1) * 44);
  receiver.onByte(wire[23], 5801); receiver.poll(5977);
  EXPECT_TRUE(receiver.failures() & kRdmPacketTimeExceeded);
  const uint32_t wrapped = UINT32_MAX - 50;
  receiver.begin(wrapped); receiver.onActivity(wrapped + 500);
  for (unsigned i = 0; i < 24; ++i) receiver.onByte(wire[i], wrapped + 500 + (i + 1) * 44);
  EXPECT_TRUE(receiver.poll(wrapped + 5800));
  EXPECT_EQ(receiver.result(decoded), RdmDiscoveryResult::SingleDevice);
  receiver.begin(100); receiver.onActivity(600);
  for (unsigned i = 0; i < 1000; ++i) receiver.onByte(0xFE, 644 + i * 44);
  EXPECT_EQ(receiver.length(), 32);
  receiver.poll(45000);
  EXPECT_TRUE(receiver.failures() & nocte::dmx::core::kRdmLengthMismatch);
  // Deliberately clear a forced encoding bit, compensate the checksum so the
  // decoded UID alone would still pass: malformed masks must still be rejected.
  wire[8] &= static_cast<uint8_t>(~0x02);
  const auto maskedChecksum = rdmChecksum(wire + 8, 12);
  encodeDiscoveryByte(static_cast<uint8_t>(maskedChecksum >> 8), wire + 20);
  encodeDiscoveryByte(static_cast<uint8_t>(maskedChecksum), wire + 22);
  EXPECT_EQ(decodeRdmDiscoveryResponse(wire, 24, decoded), RdmDiscoveryResult::CollisionOrMalformed);
  uint8_t control[34] = {};
  control[RDM_IDX_CMD_CLASS] = RDM_DISC_COMMAND_RESPONSE;
  for (uint8_t pdl : {uint8_t(2), uint8_t(8)}) {
    control[RDM_IDX_PARAM_DATA_LEN] = pdl;
    control[25] = 0x0F;
    EXPECT_EQ(classifyRdmResponse(control, 26 + pdl).status, RdmCommandStatus::Ack);
    control[25] = 0x10;
    EXPECT_EQ(classifyRdmResponse(control, 26 + pdl).status, RdmCommandStatus::InvalidResponse);
    control[25] = 0;
  }
  control[RDM_IDX_PARAM_DATA_LEN] = 0;
  EXPECT_EQ(classifyRdmResponse(control, 26).status, RdmCommandStatus::InvalidResponse);
  control[RDM_IDX_PARAM_DATA_LEN] = 2;
  for (uint8_t type : {uint8_t(RDM_RESPONSE_TYPE_ACK_TIMER), uint8_t(RDM_RESPONSE_TYPE_ACK_OVERFLOW)}) {
    control[RDM_IDX_RESPONSE_TYPE] = type;
    EXPECT_EQ(classifyRdmResponse(control, 28).status, RdmCommandStatus::InvalidResponse);
  }
  control[RDM_IDX_RESPONSE_TYPE] = RDM_RESPONSE_TYPE_NACK_REASON;
  control[25] = 1;
  EXPECT_EQ(classifyRdmResponse(control, 28).status, RdmCommandStatus::Nack);
  control[25] = 0;
  EXPECT_EQ(classifyRdmResponse(control, 28).status, RdmCommandStatus::InvalidResponse);

  struct FakeBus {
    Uid uids[4] = {Uid(UINT64_C(1)), Uid(UINT64_C(2)),
      Uid(UINT64_C(0x7FF052444D01)), Uid(UINT64_C(0xFFFFFFFFFFFE))};
    bool muted[4] = {};
    bool corrupt = false, failMute = false, endless = false, failBroadcast = false;
    RdmCommandResult setRdmDiscoveryMute(const Uid& uid, bool mute) {
      if (uid.isBroadcast()) {
        for (auto& value : muted) value = mute;
        return {failBroadcast ? RdmCommandStatus::InvalidResponse : RdmCommandStatus::Sent, 0, 0, 0};
      }
      for (unsigned i = 0; i < 4; ++i) if (uids[i] == uid && !failMute) {
        muted[i] = mute; return {RdmCommandStatus::Ack, 2, 0, 0};
      }
      return {RdmCommandStatus::Timeout, 0, 0, 0};
    }
    RdmDiscoveryResult discoverRdmBranch(const Uid& low, const Uid& high, Uid* found) {
      if (endless) return RdmDiscoveryResult::CollisionOrMalformed;
      unsigned matches = 0;
      for (unsigned i = 0; i < 4; ++i) if (!muted[i] && !(uids[i] < low) && !(high < uids[i])) {
        ++matches; *found = uids[i];
      }
      return !matches ? RdmDiscoveryResult::None : matches == 1 && !corrupt
        ? RdmDiscoveryResult::SingleDevice : RdmDiscoveryResult::CollisionOrMalformed;
    }
  } bus;
  DeviceTable<4> devices;
  auto scan = scanRdmDevices(bus, devices);
  EXPECT_EQ(scan.status, RdmScanStatus::Complete);
  EXPECT_EQ(devices.count(), 4);
  for (const auto& uid : bus.uids) EXPECT_TRUE(devices.contains(uid));
  // Always-bad discovery encoding still recovers each device at its exact leaf.
  bus.corrupt = true;
  scan = scanRdmDevices(bus, devices, 1024);
  EXPECT_EQ(scan.status, RdmScanStatus::Complete);
  EXPECT_EQ(devices.count(), 4);
  bus.corrupt = false;
  DeviceTable<1> small;
  EXPECT_EQ(scanRdmDevices(bus, small).status, RdmScanStatus::CapacityExceeded);
  bus.endless = true;
  scan = scanRdmDevices(bus, devices, 120);
  EXPECT_EQ(scan.status, RdmScanStatus::TransactionLimit);
  EXPECT_EQ(scan.transactions, 120);
  bus.endless = false; bus.failMute = true;
  scan = scanRdmDevices(bus, devices, 1024);
  EXPECT_EQ(scan.status, RdmScanStatus::Unresolved);
  EXPECT_EQ(scan.unresolved, 4);
  EXPECT_EQ(devices.count(), 0);
  EXPECT_EQ(scanRdmDevices(bus, devices, 1).status, RdmScanStatus::InvalidArgument);
  bus.failBroadcast = true;
  EXPECT_EQ(scanRdmDevices(bus, devices).status, RdmScanStatus::TransportError);
}

void testUidAndDeviceTable() {
  using namespace nocte::dmx::core;
  Uid uid(UINT64_C(0x7FF052444D01));
  char text[14];
  EXPECT_TRUE(uid.format(text, sizeof(text)));
  EXPECT_TRUE(std::strcmp(text, "7FF0:52444D01") == 0);
  EXPECT_EQ(Uid(uid.data()).value(), uid.value());
  EXPECT_TRUE(!uid.format(text, 13));
  EXPECT_EQ(text[0], '\0');
  Uid midpoint;
  EXPECT_TRUE(midpoint.setMidpoint(Uid(UINT64_C(0xFFFFFFFFFFFD)),
                                  Uid(UINT64_C(0xFFFFFFFFFFFF))));
  EXPECT_EQ(midpoint.value(), UINT64_C(0xFFFFFFFFFFFE));
  EXPECT_TRUE(!midpoint.setMidpoint(uid, uid));
  EXPECT_TRUE(Uid(UINT64_C(0x7FF0FFFFFFFF)).isBroadcast());
  EXPECT_TRUE(!uid.isBroadcast());

  DeviceTable<2> table;
  EXPECT_TRUE(table.add(uid));
  EXPECT_TRUE(!table.add(uid));
  EXPECT_TRUE(table.append(midpoint));
  EXPECT_TRUE(!table.append(Uid(1)));
  EXPECT_EQ(table.count(), 2);
  EXPECT_TRUE(!table.remove(2));
  EXPECT_TRUE(table.remove(0));
  Uid read;
  EXPECT_TRUE(table.get(0, read));
  EXPECT_EQ(read, midpoint);
  EXPECT_TRUE(table.pop(read));
  EXPECT_TRUE(!table.pop(read));
  table.append(uid);
  table.clear();
  EXPECT_EQ(table.count(), 0);
  EXPECT_EQ(table.data()[0], 0);
}

void testResponseCorrelationAndPayload() {
  using namespace nocte::dmx;
  uint8_t request[26] = {};
  uint8_t response[28] = {};
  const core::Uid controller(UINT64_C(0x7FF000000001));
  const core::Uid fixture(UINT64_C(0x7FF052444D01));
  core::initializeRdmControllerHeader(request, 24, controller.data(), 255, 1, 2);
  std::memcpy(request + RDM_IDX_DESTINATION_UID, fixture.data(), 6);
  core::setRdmParameterHeader(request, RDM_GET_COMMAND, RDM_DEVICE_START_ADDR, 0);
  core::initializeRdmResponderHeader(response, 26, fixture.data(), 255, 0, 0, 2);
  std::memcpy(response + RDM_IDX_DESTINATION_UID, controller.data(), 6);
  core::setRdmParameterHeader(response, RDM_GET_COMMAND_RESPONSE, RDM_DEVICE_START_ADDR, 2);
  response[24] = 1;
  response[25] = 43;
  appendRdmChecksum(response);
  const core::RdmResponseObservation observation = {true, true, 500, 44, 316, 2144};
  EXPECT_EQ(core::validateRdmResponse(response, sizeof(response), observation), 0);
  EXPECT_TRUE(core::matchesRdmResponse(request, sizeof(request), response, sizeof(response)));
  // A correct checksum does not make a stale or misaddressed response valid.
  const uint8_t fields[] = {3, 9, 15, 18, 20, 21, 22};
  for (uint8_t field : fields) {
    response[field] ^= 1;
    appendRdmChecksum(response);
    EXPECT_TRUE(!core::matchesRdmResponse(request, sizeof(request), response, sizeof(response)));
    response[field] ^= 1;
  }
  appendRdmChecksum(response);
  uint8_t destination[5] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
  EXPECT_EQ(core::copyRdmParameterData(response, sizeof(response), destination, 5), 2);
  EXPECT_EQ(destination[0], 1);
  EXPECT_EQ(destination[1], 43);
  EXPECT_EQ(destination[2], 0xA5);
  EXPECT_EQ(core::copyRdmParameterData(response, sizeof(response), destination, 1), 1);
  EXPECT_EQ(core::copyRdmParameterData(response, sizeof(response) - 1, destination, 5), 0);
  response[RDM_IDX_PARAM_DATA_LEN] = 3;
  appendRdmChecksum(response);
  EXPECT_TRUE((core::validateRdmResponse(response, sizeof(response), observation)
      & core::kRdmInvalidParameterDataLength) != 0);
  EXPECT_EQ(core::copyRdmParameterData(response, sizeof(response), destination, 5), 0);
  // Queued-message replies intentionally report the queued PID, not 0x0020.
  core::setRdmParameterHeader(request, RDM_GET_COMMAND, 0x0020, 0);
  core::setRdmParameterHeader(response, RDM_GET_COMMAND_RESPONSE, 0x0030, 2);
  EXPECT_TRUE(core::matchesRdmResponse(request, sizeof(request), response, sizeof(response)));
  response[RDM_IDX_RESPONSE_TYPE] = RDM_RESPONSE_TYPE_NACK_REASON;
  EXPECT_TRUE(!core::matchesRdmResponse(request, sizeof(request), response, sizeof(response)));
  response[RDM_IDX_RESPONSE_TYPE] = RDM_RESPONSE_TYPE_ACK;
  response[RDM_IDX_TRANSACTION_NUM]++;
  EXPECT_TRUE(!core::matchesRdmResponse(request, sizeof(request), response, sizeof(response)));
}

void testPortStateIsolation() {
  nocte::dmx::core::FrameStorage first;
  nocte::dmx::core::FrameStorage second;
  nocte::dmx::core::RdmTransactionState transaction;
  first.dmx[512] = 231;
  first.receivedLength = 513;
  EXPECT_EQ(second.dmx[512], 0);
  EXPECT_EQ(second.receivedLength, 0);
  EXPECT_EQ(transaction.responseLength, 0);
  EXPECT_EQ(transaction.validationFailures, 0);
  EXPECT_EQ(transaction.request[256], 0);
}

void testDmxReceiver() {
  using namespace nocte::dmx;
  core::FrameStorage frames;
  core::DmxReceiver receiver(frames);
  receiver.onByte(0); // Unframed bytes must not start a packet.
  receiver.onByte(123);
  EXPECT_EQ(receiver.onBreak(), 0);
  receiver.onByte(0);
  receiver.onByte(77);
  EXPECT_EQ(receiver.onBreak(), 1);
  EXPECT_EQ(frames.slots, 1);
  EXPECT_EQ(frames.dmx[1], 77);
  // Nonzero start codes and empty packets cannot overwrite the last good frame.
  receiver.onByte(0xCC);
  receiver.onByte(5);
  EXPECT_EQ(receiver.onBreak(), 0);
  EXPECT_EQ(receiver.onBreak(), 0);
  receiver.onByte(0);
  for (uint16_t i = 1; i <= kMaximumSlots; ++i) receiver.onByte(static_cast<uint8_t>(i));
  EXPECT_EQ(receiver.onBreak(), 512);
  EXPECT_EQ(frames.dmx[512], 0);
  EXPECT_EQ(frames.dmx[511], 255);
  receiver.onByte(0);
  for (uint16_t i = 0; i <= kMaximumSlots; ++i) receiver.onByte(42);
  EXPECT_EQ(receiver.onBreak(), 0); // Reject the whole oversized packet.
  EXPECT_EQ(frames.dmx[511], 255);
  receiver.onByte(0);
  receiver.onByte(99);
  receiver.onError();
  EXPECT_EQ(receiver.onBreak(), 0);
  receiver.onByte(0);
  receiver.onByte(11);
  EXPECT_EQ(receiver.onBreak(), 1); // Recovery, including clearing old tail.
  EXPECT_EQ(frames.dmx[1], 11);
  EXPECT_EQ(frames.dmx[511], 0);
}

void testRdmControllerCore() {
  using namespace nocte::dmx;
  using namespace nocte::dmx::core;
  EXPECT_TRUE(qualifyUartLowPulse(176, 44, false, false)); // Leading physical BREAK.
  EXPECT_TRUE(!qualifyUartLowPulse(46, 44, true, false)); // Jittered zero-byte pulse.
  EXPECT_TRUE(!qualifyUartLowPulse(176, 44, true, false)); // Coalesced data edges.
  EXPECT_TRUE(qualifyUartLowPulse(64, 44, true, true)); // Still validate/reject real short BREAK.
  EXPECT_TRUE(qualifyUartLowPulse(176, 44, true, true));
  EXPECT_TRUE(!qualifyUartLowPulse(36, 44, true, true));
  const Uid controller(UINT64_C(0x7FF000000002)), target(UINT64_C(0x7FF052444D01));
  uint8_t request[rdm::kMaximumFrameSize] = {};
  uint8_t response[rdm::kMaximumFrameSize] = {};
  const uint16_t requestLength = buildRdmRequest(request, controller.data(), target.data(),
      17, RDM_GET_COMMAND, 0x00F0, nullptr, 0);
  EXPECT_EQ(requestLength, 26);
  EXPECT_TRUE(validateRDMPacket(request));
  EXPECT_EQ(buildRdmRequest(request, controller.data(), target.data(), 17,
      RDM_SET_COMMAND, 0x00F0, nullptr, 1), 0);
  EXPECT_EQ(buildRdmRequest(request, controller.data(), target.data(), 17,
      RDM_GET_COMMAND, 0, response, 232), 0);
  initializeRdmResponderHeader(response, 26, target.data(), 17, 0, 0, 0);
  memcpy(response + RDM_IDX_DESTINATION_UID, controller.data(), 6);
  setRdmParameterHeader(response, RDM_GET_COMMAND_RESPONSE, 0x00F0, 2);
  response[24] = 0; response[25] = 42;
  appendRdmChecksum(response);
  RdmReceiver receiver;
  auto feed = [&](uint32_t end, uint32_t spacing, uint32_t gap = 0,
                  uint32_t breakUs = 176, uint32_t mabUs = 12) {
    receiver.begin(end);
    receiver.onBreak(end + spacing, end + spacing + breakUs);
    receiver.onStartBit(end + spacing + breakUs + mabUs);
    for (uint16_t i = 0; i < 28; ++i)
      receiver.onByte(response[i], end + spacing + breakUs + mabUs + 44 * (i + 1) + gap * i);
  };
  feed(100, 176);
  EXPECT_TRUE(!receiver.poll(receiver.lastByteUs() + 2144));
  EXPECT_TRUE(receiver.poll(receiver.lastByteUs() + 2145));
  EXPECT_EQ(receiver.validate(request, requestLength), 0);
  EXPECT_EQ(classifyRdmResponse(receiver.data(), receiver.length()).status, RdmCommandStatus::Ack);
  // The caller can resume late; never rebase request EOP to its wake-up time.
  feed(100, 500);
  EXPECT_TRUE(receiver.poll(9100));
  EXPECT_EQ(receiver.timing().responseSpacingUs, 500);
  EXPECT_EQ(receiver.validate(request, requestLength), 0);
  feed(UINT32_MAX - 100, 2800); // All timestamp arithmetic survives wraparound.
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_EQ(receiver.validate(request, requestLength), 0);
  feed(100, 175);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmResponseTooEarly);
  feed(100, 2801);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmResponseTooLate);
  feed(100, 500, 2101);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmInterSlotTimeout);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmPacketTimeExceeded);
  for (uint32_t breakUs : {88u, 352u}) {
    for (uint32_t mab : {8u, 88u}) {
      feed(100, 500, 0, breakUs, mab);
      receiver.poll(receiver.lastByteUs() + 2145);
      EXPECT_EQ(receiver.validate(request, requestLength), 0);
    }
  }
  feed(100, 500, 0, 353);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmInvalidPhysicalTiming);
  feed(100, 500, 0, 176, 7);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmInvalidPhysicalTiming);
  feed(100, 500);
  receiver.onByte(99, receiver.lastByteUs() + 44);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmLengthMismatch);
  receiver.begin(100);
  EXPECT_TRUE(!receiver.poll(3099));
  EXPECT_TRUE(receiver.poll(3100));
  EXPECT_EQ(classifyRdmResponse(receiver.data(), receiver.length(),
      receiver.validate(request, requestLength)).status, RdmCommandStatus::Timeout);
  receiver.begin(100);
  for (uint16_t i = 0; i < 1000; ++i) receiver.onByte(0xAA, 500 + i * 44);
  EXPECT_EQ(receiver.length(), rdm::kMaximumFrameSize);
  receiver.poll(receiver.lastByteUs() + 2145);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmMissingBreak);
  EXPECT_TRUE(receiver.validate(request, requestLength) & kRdmLengthMismatch);
  response[RDM_IDX_RESPONSE_TYPE] = RDM_RESPONSE_TYPE_NACK_REASON;
  EXPECT_EQ(classifyRdmResponse(response, 28).status, RdmCommandStatus::Nack);
  response[RDM_IDX_PARAM_DATA_LEN] = 1;
  EXPECT_EQ(classifyRdmResponse(response, 28).status, RdmCommandStatus::InvalidResponse);
  response[RDM_IDX_PARAM_DATA_LEN] = 2;
  response[RDM_IDX_RESPONSE_TYPE] = 99;
  EXPECT_TRUE(classifyRdmResponse(response, 28).validationFailures & kRdmUnexpectedResponse);
  // Longest legal packet plus latest SOP and a quiet closure must not hit the
  // absolute watchdog before completion (it needs more than 35 ms in total).
  initializeRdmResponderHeader(response, 255, target.data(), 17, 0, 0, 0);
  setRdmParameterHeader(response, RDM_GET_COMMAND_RESPONSE, 0x00F0, 231);
  appendRdmChecksum(response);
  receiver.begin(100);
  receiver.onBreak(2900, 3252);
  receiver.onStartBit(3340);
  for (uint16_t i = 0; i < 257; ++i) receiver.onByte(response[i], 3340 + 44 * (i + 1) + 76 * i);
  EXPECT_TRUE(!receiver.poll(receiver.lastByteUs() + 2144));
  EXPECT_TRUE(receiver.poll(receiver.lastByteUs() + 2145));
  EXPECT_EQ(receiver.validate(request, requestLength), 0);
  uint8_t payload[231] = {};
  EXPECT_EQ(buildRdmRequest(request, controller.data(), target.data(), 17,
      RDM_SET_COMMAND, 0x00F0, payload, sizeof(payload)), 257);
  EXPECT_TRUE(validateRDMPacket(request));
  // A legal response starting at 2800 us may still be in its BREAK when the
  // missing-response timer reaches 3000 us. Leading-edge observation must keep
  // the receiver alive until the physical BREAK can be qualified at its end.
  receiver.begin(100);
  receiver.onLow(2900);
  EXPECT_TRUE(!receiver.poll(3100));
  EXPECT_TRUE(!receiver.poll(3252));
  receiver.onHigh();
  receiver.onBreak(2900, 3252);
  receiver.onStartBit(3340);
  EXPECT_TRUE(!receiver.poll(3340));
}

void testRawGatewayRequests() {
  using namespace nocte::dmx;
  using namespace core;
  Uid source(UINT64_C(0x123456789ABC)), target(UINT64_C(0x432112345678));
  uint8_t packet[rdm::kMaximumFrameSize] = {};
  uint8_t payload[231];
  memset(payload, 0xFF, sizeof(payload));
  const uint16_t length = buildRdmRequest(packet, source.data(), target.data(), 0xFA,
      RDM_SET_COMMAND, 0x8001, payload, sizeof(payload), 0x1234);
  packet[RDM_IDX_PORT] = 7;
  appendRdmChecksum(packet);
  EXPECT_EQ(length, 257);
  EXPECT_TRUE(isRdmControllerRequest(packet, length));
  EXPECT_EQ(packet[RDM_IDX_TRANSACTION_NUM], 0xFA);
  EXPECT_EQ(packet[RDM_IDX_PORT], 7);
  EXPECT_TRUE(!isRdmControllerRequest(nullptr, length));
  EXPECT_TRUE(!isRdmControllerRequest(packet, length - 1));
  packet[256] ^= 1;
  EXPECT_TRUE(!isRdmControllerRequest(packet, length));
  appendRdmChecksum(packet);
  memset(packet + RDM_IDX_DESTINATION_UID, 0xFF, 6);
  appendRdmChecksum(packet);
  EXPECT_TRUE(isRdmControllerRequest(packet, length)); // Broadcast SET.
  packet[RDM_IDX_CMD_CLASS] = RDM_GET_COMMAND;
  appendRdmChecksum(packet);
  EXPECT_TRUE(!isRdmControllerRequest(packet, length)); // Never broadcast GET.
  packet[RDM_IDX_CMD_CLASS] = RDM_DISCOVERY_COMMAND;
  appendRdmChecksum(packet);
  EXPECT_TRUE(!isRdmControllerRequest(packet, length)); // Use discovery API.
  packet[RDM_IDX_CMD_CLASS] = RDM_SET_COMMAND;
  packet[RDM_IDX_PORT] = 0;
  appendRdmChecksum(packet);
  EXPECT_TRUE(!isRdmControllerRequest(packet, length));
  packet[RDM_IDX_PORT] = 7;
  packet[RDM_IDX_MSG_COUNT] = 1;
  appendRdmChecksum(packet);
  EXPECT_TRUE(!isRdmControllerRequest(packet, length));
  packet[RDM_IDX_MSG_COUNT] = 0;
  memset(packet + RDM_IDX_SOURCE_UID, 0xFF, 6);
  appendRdmChecksum(packet);
  EXPECT_TRUE(!isRdmControllerRequest(packet, length));

  uint8_t response[28] = {};
  initializeRdmResponderHeader(response, 26, target.data(), 0xFA,
      RDM_RESPONSE_TYPE_ACK_TIMER_HI_RES, 0, 0);
  setRdmParameterHeader(response, RDM_GET_COMMAND_RESPONSE, RDM_DEVICE_INFO, 2);
  EXPECT_EQ(classifyRdmResponse(response, sizeof(response)).status, RdmCommandStatus::Deferred);
  response[23] = 1;
  EXPECT_EQ(classifyRdmResponse(response, sizeof(response)).status, RdmCommandStatus::InvalidResponse);
}

}  // namespace

int main() {
  testDmxFrameLimits();
  testDmxFrameCopying();
  testRdmPacketConstruction();
  testRdmResponseValidation();
  testRdmDiscoveryDecoding();
  testDiscoveryReceiverAndScan();
  testUidAndDeviceTable();
  testResponseCorrelationAndPayload();
  testPortStateIsolation();
  testDmxReceiver();
  testRdmControllerCore();
  testRawGatewayRequests();

  if (failures != 0) {
    std::cerr << failures << " NocteDMX core assertion(s) failed\n";
    return 1;
  }
  std::cout << "All NocteDMX core tests passed\n";
  return 0;
}
