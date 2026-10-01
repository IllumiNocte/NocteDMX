#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#include <nocte/core/Constants.h>
#include <nocte/core/DmxFrame.h>
#include <nocte/core/RdmPacket.h>
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
  std::fill(frame, frame + sizeof(frame), 0xFF);

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

  std::fill(packet, packet + sizeof(packet), 0);
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
  std::fill(response, response + 7, RDM_DISC_PREAMBLE);
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

  response[23] ^= 0x01;
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(
                response, sizeof(response), decoded),
            RdmDiscoveryResult::CollisionOrMalformed);
  EXPECT_EQ(core::decodeRdmDiscoveryResponse(
                response, sizeof(response) - 1, decoded),
            RdmDiscoveryResult::CollisionOrMalformed);
}

}  // namespace

int main() {
  testDmxFrameLimits();
  testDmxFrameCopying();
  testRdmPacketConstruction();
  testRdmResponseValidation();
  testRdmDiscoveryDecoding();

  if (failures != 0) {
    std::cerr << failures << " NocteDMX core assertion(s) failed\n";
    return 1;
  }
  std::cout << "All NocteDMX core tests passed\n";
  return 0;
}
