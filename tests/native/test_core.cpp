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

}  // namespace

int main() {
  testDmxFrameLimits();
  testDmxFrameCopying();
  testRdmPacketConstruction();
  testRdmResponseValidation();
  testRdmDiscoveryDecoding();
  testUidAndDeviceTable();
  testResponseCorrelationAndPayload();
  testPortStateIsolation();

  if (failures != 0) {
    std::cerr << failures << " NocteDMX core assertion(s) failed\n";
    return 1;
  }
  std::cout << "All NocteDMX core tests passed\n";
  return 0;
}
