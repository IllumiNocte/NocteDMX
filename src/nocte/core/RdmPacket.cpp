#include "RdmPacket.h"

#include <string.h>

#include <rdm/rdm_utility.h>

namespace nocte {
namespace dmx {
namespace core {

uint16_t rdmWireLength(uint8_t messageLength) {
  return static_cast<uint16_t>(messageLength) + rdm::kChecksumSize;
}

uint16_t buildRdmRequest(uint8_t* packet, const uint8_t* sourceUid,
    const uint8_t* destinationUid, uint8_t transaction, uint8_t commandClass,
    uint16_t pid, const uint8_t* data, uint16_t length, uint16_t subDevice) {
  if (!packet || !sourceUid || !destinationUid || (length && !data)
      || length > rdm::kMaximumParameterDataLength
      || (commandClass != RDM_GET_COMMAND && commandClass != RDM_SET_COMMAND)) return 0;
  const uint8_t messageLength = static_cast<uint8_t>(rdm::kMinimumMessageLength + length);
  initializeRdmControllerHeader(packet, messageLength, sourceUid, transaction, 1, subDevice);
  memcpy(packet + RDM_IDX_DESTINATION_UID, destinationUid, rdm::kUidSize);
  setRdmParameterHeader(packet, commandClass, pid, static_cast<uint8_t>(length));
  if (length) memcpy(packet + RDM_PKT_BASE_MSG_LEN, data, length);
  const uint16_t checksum = rdmChecksum(packet, messageLength);
  packet[messageLength] = static_cast<uint8_t>(checksum >> 8);
  packet[messageLength + 1] = static_cast<uint8_t>(checksum);
  return rdmWireLength(messageLength);
}

RdmCommandResult classifyRdmResponse(const uint8_t* response, uint16_t length,
                                    uint16_t failures) {
  if (!length && !failures) return {RdmCommandStatus::Timeout, 0, 0, 0};
  if (failures || !response || length < RDM_PKT_BASE_TOTAL_LEN)
    return {RdmCommandStatus::InvalidResponse, 0, 0, failures};
  const uint8_t pdl = response[RDM_IDX_PARAM_DATA_LEN];
  RdmCommandStatus status = RdmCommandStatus::InvalidResponse;
  switch (response[RDM_IDX_RESPONSE_TYPE]) {
    case RDM_RESPONSE_TYPE_ACK: status = RdmCommandStatus::Ack; break;
    case RDM_RESPONSE_TYPE_NACK_REASON:
      if (pdl == 2) status = RdmCommandStatus::Nack;
      break;
    case RDM_RESPONSE_TYPE_ACK_TIMER:
      if (pdl == 2) status = RdmCommandStatus::Deferred;
      break;
    case RDM_RESPONSE_TYPE_ACK_OVERFLOW: status = RdmCommandStatus::Overflow; break;
  }
  if (status == RdmCommandStatus::InvalidResponse) failures |= kRdmUnexpectedResponse;
  return {status, pdl, 0, failures};
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
  if (packet && receivedLength >= RDM_PKT_BASE_TOTAL_LEN
      && messageLength >= RDM_PKT_BASE_MSG_LEN
      && packet[RDM_IDX_PARAM_DATA_LEN] != messageLength - RDM_PKT_BASE_MSG_LEN) {
    failures |= kRdmInvalidParameterDataLength;
  }

  if (failures == kRdmResponseValid && !validateRDMPacket(packet)) {
    failures |= kRdmChecksumMismatch;
  }
  return failures;
}

bool matchesRdmResponse(const uint8_t* request, uint16_t requestLength,
                        const uint8_t* response, uint16_t responseLength) {
  if (!request || !response || requestLength < RDM_PKT_BASE_TOTAL_LEN
      || responseLength < RDM_PKT_BASE_TOTAL_LEN) return false;
  // E1.20-2025 10.3.1: a GET:QUEUED_MESSAGE can carry a queued PID or an
  // empty STATUS_MESSAGES reply. Do not reject that legal PID change.
  const bool queuedMessage = request[RDM_IDX_CMD_CLASS] == RDM_GET_COMMAND
      && request[RDM_IDX_PID_MSB] == 0 && request[RDM_IDX_PID_LSB] == 0x20
      && response[RDM_IDX_RESPONSE_TYPE] != RDM_RESPONSE_TYPE_NACK_REASON;
  return memcmp(request + RDM_IDX_DESTINATION_UID,
                response + RDM_IDX_SOURCE_UID, rdm::kUidSize) == 0
      && memcmp(request + RDM_IDX_SOURCE_UID,
                response + RDM_IDX_DESTINATION_UID, rdm::kUidSize) == 0
      && request[RDM_IDX_TRANSACTION_NUM] == response[RDM_IDX_TRANSACTION_NUM]
      && response[RDM_IDX_CMD_CLASS] == request[RDM_IDX_CMD_CLASS] + 1
      && (queuedMessage || memcmp(request + RDM_IDX_PID_MSB,
                                   response + RDM_IDX_PID_MSB, 2) == 0)
      && memcmp(request + RDM_IDX_SUB_DEV_MSB, response + RDM_IDX_SUB_DEV_MSB, 2) == 0;
}

uint8_t copyRdmParameterData(const uint8_t* packet, uint16_t length,
                             uint8_t* destination, uint16_t capacity) {
  if (!packet || !destination || length < RDM_PKT_BASE_TOTAL_LEN) return 0;
  const uint8_t pdl = packet[RDM_IDX_PARAM_DATA_LEN];
  if (pdl > rdm::kMaximumParameterDataLength
      || packet[RDM_IDX_PACKET_SIZE] != RDM_PKT_BASE_MSG_LEN + pdl
      || length != rdmWireLength(packet[RDM_IDX_PACKET_SIZE])) return 0;
  const uint8_t copied = static_cast<uint8_t>(capacity < pdl ? capacity : pdl);
  memcpy(destination, packet + RDM_PKT_BASE_MSG_LEN, copied);
  return copied;
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
