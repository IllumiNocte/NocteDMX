#include "RdmPacket.h"

#include <string.h>

#include <rdm/rdm_utility.h>

namespace nocte {
namespace dmx {
namespace core {

uint16_t rdmWireLength(uint8_t messageLength) {
  return static_cast<uint16_t>(messageLength) + rdm::kChecksumSize;
}

void initializeRdmControllerHeader(
    uint8_t* packet,
    uint8_t messageLength,
    const uint8_t* sourceUid,
    uint8_t transactionNumber,
    uint8_t port,
    uint16_t subDevice) {
  if (!packet || !sourceUid) {
    return;
  }

  packet[RDM_IDX_START_CODE] = rdm::kStartCode;
  packet[RDM_IDX_SUB_START_CODE] = rdm::kSubStartCode;
  packet[RDM_IDX_PACKET_SIZE] = messageLength;
  memcpy(packet + RDM_IDX_SOURCE_UID, sourceUid, rdm::kUidSize);
  packet[RDM_IDX_TRANSACTION_NUM] = transactionNumber;
  packet[RDM_IDX_PORT] = port;
  packet[RDM_IDX_MSG_COUNT] = 0;
  packet[RDM_IDX_SUB_DEV_MSB] = static_cast<uint8_t>(subDevice >> 8);
  packet[RDM_IDX_SUB_DEV_LSB] = static_cast<uint8_t>(subDevice & 0xFF);
}

void initializeRdmResponderHeader(
    uint8_t* packet,
    uint8_t messageLength,
    const uint8_t* sourceUid,
    uint8_t transactionNumber,
    uint8_t responseType,
    uint8_t messageCount,
    uint16_t subDevice) {
  if (!packet || !sourceUid) {
    return;
  }

  packet[RDM_IDX_START_CODE] = rdm::kStartCode;
  packet[RDM_IDX_SUB_START_CODE] = rdm::kSubStartCode;
  packet[RDM_IDX_PACKET_SIZE] = messageLength;
  memcpy(packet + RDM_IDX_SOURCE_UID, sourceUid, rdm::kUidSize);
  packet[RDM_IDX_TRANSACTION_NUM] = transactionNumber;
  packet[RDM_IDX_RESPONSE_TYPE] = responseType;
  packet[RDM_IDX_MSG_COUNT] = messageCount;
  packet[RDM_IDX_SUB_DEV_MSB] = static_cast<uint8_t>(subDevice >> 8);
  packet[RDM_IDX_SUB_DEV_LSB] = static_cast<uint8_t>(subDevice & 0xFF);
}

void setRdmParameterHeader(
    uint8_t* packet,
    uint8_t commandClass,
    uint16_t parameterId,
    uint8_t parameterDataLength) {
  if (!packet) {
    return;
  }

  packet[RDM_IDX_CMD_CLASS] = commandClass;
  packet[RDM_IDX_PID_MSB] = static_cast<uint8_t>(parameterId >> 8);
  packet[RDM_IDX_PID_LSB] = static_cast<uint8_t>(parameterId & 0xFF);
  packet[RDM_IDX_PARAM_DATA_LEN] = parameterDataLength;
}

uint16_t validateRdmResponse(
    const uint8_t* packet,
    uint16_t receivedLength,
    const RdmResponseObservation& observation) {
  uint16_t failures = kRdmResponseValid;

  if (!observation.breakSeen) failures |= kRdmMissingBreak;
  if (!observation.frameClosed) failures |= kRdmFrameNotClosed;
  if (observation.firstSlotDelayUs < observation.minimumFirstSlotDelayUs) {
    failures |= kRdmResponseTooEarly;
  }
  if (observation.maximumSlotIntervalUs
      > observation.maximumSlotIntervalAllowedUs) {
    failures |= kRdmInterSlotTimeout;
  }
  if (!packet || receivedLength < RDM_PKT_BASE_TOTAL_LEN) {
    failures |= kRdmFrameTooShort;
  }

  const bool hasHeader = packet && receivedLength >= 3;
  if (!hasHeader || packet[RDM_IDX_START_CODE] != rdm::kStartCode) {
    failures |= kRdmInvalidStartCode;
  }
  if (!hasHeader || packet[RDM_IDX_SUB_START_CODE] != rdm::kSubStartCode) {
    failures |= kRdmInvalidSubStartCode;
  }

  const uint8_t messageLength =
      hasHeader ? packet[RDM_IDX_PACKET_SIZE] : 0;
  if (messageLength < rdm::kMinimumMessageLength) {
    failures |= kRdmInvalidMessageLength;
  }
  if (!hasHeader || receivedLength != rdmWireLength(messageLength)) {
    failures |= kRdmLengthMismatch;
  }

  if (failures == kRdmResponseValid && !validateRDMPacket(packet)) {
    failures |= kRdmChecksumMismatch;
  }
  return failures;
}

RdmDiscoveryResult decodeRdmDiscoveryResponse(
    const uint8_t* response,
    uint16_t responseLength,
    uint8_t* discoveredUid) {
  if (!response || responseLength == 0) {
    return RdmDiscoveryResult::None;
  }

  uint8_t separator = 0;
  const uint8_t searchLength = responseLength < 8
      ? static_cast<uint8_t>(responseLength)
      : 8;
  for (; separator < searchLength; ++separator) {
    if (response[separator] == RDM_DISC_PREAMBLE_SEPARATOR) {
      break;
    }
  }

  if (separator >= searchLength || responseLength != separator + 17) {
    return RdmDiscoveryResult::CollisionOrMalformed;
  }

  const uint8_t* encoded = response + separator + 1;
  const uint16_t encodedUidChecksum = rdmChecksum(encoded, 12);
  uint8_t payload[8];
  for (uint8_t index = 0; index < 8; ++index) {
    payload[index] = encoded[index * 2] & encoded[index * 2 + 1];
  }

  if (!testRDMChecksum(encodedUidChecksum, payload, 6)) {
    return RdmDiscoveryResult::CollisionOrMalformed;
  }

  if (discoveredUid) {
    memcpy(discoveredUid, payload, rdm::kUidSize);
  }
  return RdmDiscoveryResult::SingleDevice;
}

}  // namespace core
}  // namespace dmx
}  // namespace nocte
