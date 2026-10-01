// Direct 3.3-V UART bench: S3 GPIO17 TX -> RP2040 GPIO1 RX,
// S3 GPIO18 RX <- RP2040 GPIO0 TX; common GND, no RE/DE or RS485.
// USB CDC commands: input/output/output24/stop/status/frame/quiet/verbose.
#include <NocteDMX.h>

namespace {
nocte::dmx::Port port;
std::atomic<bool> available{false};
char command[16] = {};
uint8_t commandLength = 0;
bool discardCommand = false;
uint8_t pattern[512] = {};
uint8_t received[512] = {};
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
    Serial.println("Commands: input / output / output24 / stop / status / frame / quiet / verbose");
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
