#pragma once

#include <stdint.h>

#include "Constants.h"

namespace nocte {
namespace dmx {
namespace core {

enum RdmResponseValidationFailure : uint16_t {
  kRdmResponseValid = 0,
  kRdmMissingBreak = 1u << 0,
  kRdmFrameNotClosed = 1u << 1,
  kRdmResponseTooEarly = 1u << 2,
  kRdmInterSlotTimeout = 1u << 3,
  kRdmFrameTooShort = 1u << 4,
  kRdmInvalidStartCode = 1u << 5,
  kRdmInvalidSubStartCode = 1u << 6,
  kRdmInvalidMessageLength = 1u << 7,
  kRdmLengthMismatch = 1u << 8,
  kRdmChecksumMismatch = 1u << 9,
  kRdmInvalidParameterDataLength = 1u << 10,
  kRdmUnexpectedResponse = 1u << 11,
};

struct RdmResponseObservation {
  bool breakSeen;
  bool frameClosed;
  uint32_t firstSlotDelayUs;
  uint32_t maximumSlotIntervalUs;
  uint32_t minimumFirstSlotDelayUs;
  uint32_t maximumSlotIntervalAllowedUs;
};

enum class RdmDiscoveryResult : uint8_t {
  None = 0,
  CollisionOrMalformed = 1,
  SingleDevice = 2,
};

enum class RdmCommandStatus : uint8_t {
  Ack, Nack, Deferred, Overflow, Timeout, InvalidResponse, InvalidArgument,
};

struct RdmCommandResult {
  RdmCommandStatus status;
  uint8_t parameterDataLength;
  uint8_t copiedLength;
  uint16_t validationFailures;
  bool ok() const { return status == RdmCommandStatus::Ack; }
};

uint16_t rdmWireLength(uint8_t messageLength);

void initializeRdmControllerHeader(
    uint8_t* packet,
    uint8_t messageLength,
    const uint8_t* sourceUid,
    uint8_t transactionNumber,
    uint8_t port,
    uint16_t subDevice);

void initializeRdmResponderHeader(
    uint8_t* packet,
    uint8_t messageLength,
    const uint8_t* sourceUid,
    uint8_t transactionNumber,
    uint8_t responseType,
    uint8_t messageCount,
    uint16_t subDevice);

void setRdmParameterHeader(
    uint8_t* packet,
    uint8_t commandClass,
    uint16_t parameterId,
    uint8_t parameterDataLength);

uint16_t validateRdmResponse(
    const uint8_t* packet,
    uint16_t receivedLength,
    const RdmResponseObservation& observation);

// Used only after packet validation. Compares both UIDs, transaction,
// command class, PID and sub-device to the outstanding request. Queued-message
// ACKs may report a different PID as required by E1.20.
bool matchesRdmResponse(const uint8_t* request, uint16_t requestLength,
                        const uint8_t* response, uint16_t responseLength);

// Copies only actual PDL bytes; never consumes checksum or stale buffer tail.
uint8_t copyRdmParameterData(const uint8_t* packet, uint16_t length,
                             uint8_t* destination, uint16_t capacity);

RdmDiscoveryResult decodeRdmDiscoveryResponse(
    const uint8_t* response,
    uint16_t responseLength,
    uint8_t* discoveredUid);

}  // namespace core
}  // namespace dmx
}  // namespace nocte
