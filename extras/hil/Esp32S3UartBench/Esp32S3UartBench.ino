// Direct 3.3-V UART bench: S3 GPIO17 TX -> RP2040 GPIO1 RX,
// S3 GPIO18 RX <- RP2040 GPIO0 TX; common GND, no RE/DE or RS485.
// USB CDC commands: input/output/output24/stop/status/frame/quiet/verbose.
#include <NocteDMX.h>

namespace {
nocte::dmx::Port port;
std::atomic<bool> available{false};
char command[512] = {};
uint16_t commandLength = 0;
bool discardCommand = false;
uint8_t pattern[512] = {};
uint8_t received[512] = {};
uint8_t parameterData[231] = {};
uint8_t response[257] = {};
const nocte::dmx::core::Uid fixtureUid(UINT64_C(0x7FF052444D01));
bool reportFrames = true;
const char* mode = "input";

void NOCTE_DMX_ISR_ATTR onFrame(int) { available.store(true); }

void status() {
  const auto stats = port.statistics();
  Serial.printf("{\"type\":\"status\",\"mode\":\"%s\",\"active\":%s,\"error\":%u,\"slots\":%u,\"rxFrames\":%lu,"
                "\"rxErrors\":%lu,\"txFrames\":%lu,\"txTimeouts\":%lu,\"freeHeap\":%lu}\n",
                mode, port.isActive() ? "true" : "false", static_cast<unsigned>(port.lastError()),
                port.numberOfSlots(), static_cast<unsigned long>(stats.receivedFrames),
                static_cast<unsigned long>(stats.receiveErrors),
                static_cast<unsigned long>(stats.transmittedFrames),
                static_cast<unsigned long>(stats.transmitTimeouts),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

void execute() {
  if (strcmp(command, "burst") == 0) {
    const uint32_t before = ESP.getFreeHeap();
    unsigned acked = 0, flags = 0;
    for (unsigned i = 0; i < 100; ++i) {
      const auto result = port.getRdmParameter(fixtureUid, 0x60, parameterData, sizeof(parameterData));
      flags |= result.validationFailures;
      if (!result.ok() || result.parameterDataLength != 19 || result.copiedLength != 19) break;
      ++acked;
    }
    Serial.printf("{\"type\":\"burst\",\"acked\":%u,\"flags\":%u,\"beforeHeap\":%lu,\"afterHeap\":%lu}\n",
        acked, flags, static_cast<unsigned long>(before), static_cast<unsigned long>(ESP.getFreeHeap()));
    return;
  }
  if (strcmp(command, "invalid") == 0) {
    const nocte::dmx::core::Uid broadcast(UINT64_C(0xFFFFFFFFFFFF));
    const nocte::dmx::core::RdmCommandResult results[] = {
      port.getRdmParameter(broadcast, 0x60, parameterData, sizeof(parameterData)),
      port.getRdmParameter(fixtureUid, 0x60, nullptr, 1),
      port.getRdmParameter(fixtureUid, 0x60, parameterData, sizeof(parameterData), received, 232),
      port.setRdmParameter(fixtureUid, 0xF0, received, 232),
      port.setRdmParameter(fixtureUid, 0xF0, nullptr, 1),
      port.setRdmParameter(broadcast, 0xF0, received, 2),
    };
    Serial.print("{\"type\":\"invalid\",\"statuses\":[");
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i)
      Serial.printf(i ? ",%u" : "%u", static_cast<unsigned>(results[i].status));
    Serial.println("]}");
    return;
  }
  if (strcmp(command, "rdm") == 0) {
    port.setUid(nocte::dmx::core::Uid(UINT64_C(0x7FF000000002)));
    port.startRDM(255);
    mode = "rdm-controller";
    status();
    return;
  }
  if (strncmp(command, "get ", 4) == 0 || strncmp(command, "set ", 4) == 0) {
    unsigned pid = 0, capacity = sizeof(parameterData);
    nocte::dmx::core::RdmCommandResult result;
    memset(parameterData, 0xA5, sizeof(parameterData));
    if (command[0] == 'g') {
      sscanf(command + 4, "%x %u", &pid, &capacity);
      if (capacity > sizeof(parameterData)) capacity = sizeof(parameterData);
      result = port.getRdmParameter(fixtureUid, pid, parameterData, capacity);
    } else {
      char* payload = nullptr;
      pid = strtoul(command + 4, &payload, 16);
      while (*payload == ' ') ++payload;
      uint16_t length = 0;
      bool valid = strlen(payload) % 2 == 0 && strlen(payload) <= sizeof(parameterData) * 2;
      for (; valid && *payload; payload += 2) {
        char pair[] = {payload[0], payload[1], 0};
        char* end = nullptr;
        const unsigned value = strtoul(pair, &end, 16);
        if (*end) { valid = false; break; }
        received[length++] = static_cast<uint8_t>(value);
      }
      if (valid) result = port.setRdmParameter(fixtureUid, pid, received, length);
      else result = {nocte::dmx::core::RdmCommandStatus::InvalidArgument, 0, 0, 0};
    }
    const uint16_t wireLength = port.copyRdmResponse(response, sizeof(response));
    Serial.printf("{\"type\":\"rdm\",\"status\":%u,\"pdl\":%u,\"copied\":%u,\"flags\":%u,\"guard\":%u,\"data\":[",
        static_cast<unsigned>(result.status), result.parameterDataLength,
        result.copiedLength, result.validationFailures,
        result.copiedLength < sizeof(parameterData) ? parameterData[result.copiedLength] : 165);
    for (uint16_t i = 0; i < result.copiedLength; ++i)
      Serial.printf(i ? ",%u" : "%u", parameterData[i]);
    Serial.print("],\"wire\":[");
    for (uint16_t i = 0; i < wireLength; ++i) Serial.printf(i ? ",%u" : "%u", response[i]);
    const auto timing = port.rdmReceiveTiming();
    Serial.printf("],\"timing\":{\"breakUs\":%lu,\"mabUs\":%lu,\"spacingUs\":%lu,\"maxIntervalUs\":%lu,\"packetUs\":%lu}}\n",
        static_cast<unsigned long>(timing.breakUs), static_cast<unsigned long>(timing.mabUs),
        static_cast<unsigned long>(timing.responseSpacingUs),
        static_cast<unsigned long>(timing.maximumSlotIntervalUs),
        static_cast<unsigned long>(timing.packetUs));
    return;
  }
  if (strcmp(command, "input") == 0) { port.startInput(); mode = "input"; }
  else if (strcmp(command, "output") == 0 || strcmp(command, "output24") == 0) {
    port.stop();
    available.store(false);
    port.setFrame(pattern, strcmp(command, "output24") == 0 ? 24 : 512);
    port.startOutput();
    mode = "output";
  } else if (strcmp(command, "stop") == 0) {
    port.stop(); available.store(false); mode = "stopped";
  } else if (strcmp(command, "frame") == 0) {
    const uint16_t slots = port.copyFrame(received, sizeof(received));
    Serial.printf("{\"type\":\"frame\",\"slots\":%u,\"values\":[", slots);
    for (uint16_t i = 0; i < slots; ++i) Serial.printf(i ? ",%u" : "%u", received[i]);
    Serial.println("]}");
    return;
  } else if (strcmp(command, "quiet") == 0) { reportFrames = false; }
  else if (strcmp(command, "verbose") == 0) { reportFrames = true; }
  else if (strcmp(command, "status") != 0) {
    Serial.println("Commands: input / output / output24 / rdm / get HEX_PID [capacity] / set HEX_PID HEX_DATA / stop / status / frame / quiet / verbose");
  }
  status();
}
}

void setup() {
  Serial.begin(115200);
  for (uint16_t i = 0; i < 512; ++i) pattern[i] = static_cast<uint8_t>(i * 17 + 3);
  port.setPins(17, 18);
  port.setDirectionPin(255);
  port.setDataReceivedCallback(onFrame);
  port.startInput();
  Serial.println("NocteDMX S3 UART bench: TX=17 RX=18, input mode, no RE/DE");
}

void loop() {
  while (Serial.available()) {
    const char byte = static_cast<char>(Serial.read());
    if (byte == '\r') continue;
    if (byte == '\n') {
      if (!discardCommand) {
        command[commandLength] = '\0';
        execute();
      }
      commandLength = 0;
      discardCommand = false;
    } else if (discardCommand) {
      continue;
    } else if (commandLength < sizeof(command) - 1) {
      command[commandLength++] = byte;
    } else {
      commandLength = 0;
      discardCommand = true;
      Serial.println("Command too long");
    }
  }
  // Callback only signals availability. Snapshot/report outside the ISR.
  if (available.exchange(false)) {
    const uint16_t slots = port.copyFrame(received, sizeof(received));
    bool matches = slots == 512;
    for (uint16_t i = 0; i < slots; ++i) if (received[i] != pattern[i]) matches = false;
    static uint32_t lastReport = 0;
    if (reportFrames && millis() - lastReport >= 1000) {
      Serial.printf("RX slots=%u first=%u last=%u patternMatch=%s\n", slots,
                    slots ? received[0] : 0, slots ? received[slots - 1] : 0,
                    matches ? "true" : "false");
      status();
      lastReport = millis();
    }
  }
  delay(1);
}
