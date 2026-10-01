// Standalone HIL firmware. Results travel in a DMX frame because UART0 is
// occupied by the protocol under test. Never use this on a show/live fixture:
// it changes the RP2040 fixture address to 43, restores 42 and toggles Identify.
#include <NocteDMX.h>
#include <rdm/UID.h>
#include <rdm/rdm_utility.h>

#ifndef NOCTE_HIL_DIRECTION_PIN
#define NOCTE_HIL_DIRECTION_PIN 5
#endif

namespace {
nocte::dmx::Port port;
uint8_t frame[512] = {};
uint8_t cycle = 0;
bool exclusive = false;
}

void setup() {
  port.setUid(nocte::dmx::core::Uid(UINT64_C(0x7FF000000002)));
  port.startRDM(NOCTE_HIL_DIRECTION_PIN, nocte::dmx::PortMode::Send);
  // The legacy default port must neither steal nor stop this UART owner.
  {
    nocte::dmx::Port other;
    other.startOutput();
    other.startRDM(NOCTE_HIL_DIRECTION_PIN, nocte::dmx::PortMode::Receive);
    other.setTaskReceive();
    other.restoreTaskSendDMX();
    other.sendRawRDMPacket(26);
    exclusive = port.isActive() && !other.isActive();
  }
  exclusive = exclusive && port.isActive();
}

void loop() {
  UID lower(UINT64_C(0));
  UID upper(UINT64_C(0xFFFFFFFFFFFF));
  UID found;
  memset(frame, 0, sizeof(frame));
  frame[0] = 0xA5;
  frame[1] = 0x4E;
  frame[2] = 1; // telemetry version
  frame[3] = ++cycle;
  frame[4] = exclusive;
  port.sendRDMDiscoveryMute(BROADCAST_ALL_DEVICES_ID, RDM_DISC_UNMUTE);
  frame[5] = port.sendRDMDiscoveryPacket(lower, upper, &found);
  frame[6] = port.lastRDMDiscoveryResponseLength();
  memcpy(frame + 24, found.rawbytes(), 6);
  if (frame[5] == RDM_DID_DISCOVER) {
    frame[7] = port.sendRDMDiscoveryMute(found, RDM_DISC_MUTE);
    uint8_t info[25];
    memset(info, 0xA5, sizeof(info));
    const auto details = port.getRdmParameter(found, RDM_DEVICE_INFO, info, sizeof(info));
    frame[8] = details.ok();
    frame[9] = details.parameterDataLength;
    frame[10] = details.copiedLength;
    frame[11] = info[19] == 0xA5 && info[24] == 0xA5;
    memcpy(frame + 32, info, 19);
    uint8_t address[2] = {};
    frame[12] = port.getRdmParameter(found, RDM_DEVICE_START_ADDR, address, 2).ok();
    const uint8_t updated[] = {0, 43};
    const uint8_t restore[] = {0, 42};
    frame[13] = port.setRdmParameter(found, RDM_DEVICE_START_ADDR, updated, 2).ok();
    frame[14] = port.getRdmParameter(found, RDM_DEVICE_START_ADDR, address, 2).ok();
    frame[15] = address[1];
    frame[16] = port.setRdmParameter(found, RDM_DEVICE_START_ADDR, restore, 2).ok();
    const uint8_t on[] = {1};
    const uint8_t off[] = {0};
    frame[17] = port.setRdmParameter(found, RDM_IDENTIFY_DEVICE, on, 1).ok();
    frame[18] = port.setRdmParameter(found, RDM_IDENTIFY_DEVICE, off, 1).ok();
    frame[19] = static_cast<uint8_t>(port.getRdmParameter(found, 0x7FFE, address, 2).status);
    frame[20] = static_cast<uint8_t>(port.setRdmParameter(found,
        RDM_DEVICE_START_ADDR, updated, 232).status);
    frame[21] = static_cast<uint8_t>(port.lastRDMResponseValidationFailures() >> 8);
    frame[22] = static_cast<uint8_t>(port.lastRDMResponseValidationFailures());
  }
  // Nonzero guard at the final slot catches truncation of full-frame output.
  frame[511] = 0x5A;
  port.setFrame(frame, 512);
  delay(3000);
}
