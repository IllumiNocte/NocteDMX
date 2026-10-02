// Direct 3.3-V UART bench: S3 GPIO17 TX -> RP2040 GPIO1 RX,
// S3 GPIO18 RX <- RP2040 GPIO0 TX; common GND, no RE/DE or RS485.
// USB CDC: DMX/RDM commands plus load none/cpu/wifi/combined (bench only).
#include <NocteDMX.h>
#include <WiFi.h>
#include <esp_wifi.h>

#ifndef NOCTE_HIL_UART_NUMBER
#define NOCTE_HIL_UART_NUMBER 1
#endif
static_assert(NOCTE_HIL_UART_NUMBER == 1 || NOCTE_HIL_UART_NUMBER == 2,
              "S3 bench supports UART1 or UART2 only");

namespace {
nocte::dmx::Port port(NOCTE_HIL_UART_NUMBER, 17, 18);
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
const char* loadMode = "none";
std::atomic<bool> cpuLoad{false};
std::atomic<uint32_t> busyUs[2]{};
std::atomic<uint32_t> loadCycles[2]{};
TaskHandle_t loadTasks[2]{};
bool wifiLoad = false, scanRunning = false;
uint32_t scansStarted = 0, scansCompleted = 0, scanFailures = 0;

void loadWorker(void* argument) {
  // Seven-ms compute windows / three-ms yields on each core at priority 1.
  // busyUs is elapsed window time including preemption, not CPU utilization.
  const unsigned core = static_cast<unsigned>(reinterpret_cast<uintptr_t>(argument));
  volatile uint32_t value = 0x12345678;
  for (;;) {
    if (cpuLoad.load()) {
      const uint32_t start = micros();
      do {
        value = value * 1664525UL + 1013904223UL;
      } while (micros() - start < 7000);
      busyUs[core].fetch_add(micros() - start);
      loadCycles[core].fetch_add(1);
      vTaskDelay(pdMS_TO_TICKS(3));
    } else vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void serviceWifiLoad() {
  if (!wifiLoad) return;
  if (scanRunning) {
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;
    if (result >= 0) ++scansCompleted;
    else ++scanFailures;
    WiFi.scanDelete();
    scanRunning = false;
  }
  const int16_t result = WiFi.scanNetworks(true, true, false, 60);
  if (result == WIFI_SCAN_RUNNING) { ++scansStarted; scanRunning = true; }
  else ++scanFailures;
}

bool setLoad(const char* requested) {
  const bool cpu = strcmp(requested, "cpu") == 0 || strcmp(requested, "combined") == 0;
  const bool wifi = strcmp(requested, "wifi") == 0 || strcmp(requested, "combined") == 0;
  if (!cpu && !wifi && strcmp(requested, "none") != 0) return false;
  cpuLoad.store(false);
  if (cpu) {
    for (unsigned core = 0; core < 2; ++core) {
      if (!loadTasks[core] && xTaskCreatePinnedToCore(loadWorker, "Nocte HIL load", 2048,
          reinterpret_cast<void*>(static_cast<uintptr_t>(core)), 1, &loadTasks[core], core) != pdPASS)
        return false;
    }
  }
  if (wifi != wifiLoad) {
    if (wifi) {
      WiFi.persistent(false);
      if (!WiFi.mode(WIFI_AP_STA) || !WiFi.softAP("NocteDMX-HIL", "nocte-hil-load")) {
        WiFi.mode(WIFI_OFF);
        return false;
      }
    } else {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    scanRunning = false;
    wifiLoad = wifi;
  }
  loadMode = cpu ? (wifi ? "combined" : "cpu") : (wifi ? "wifi" : "none");
  cpuLoad.store(cpu);
  serviceWifiLoad();
  return true;
}

void NOCTE_DMX_ISR_ATTR onFrame(int) { available.store(true); }

void status() {
  const auto stats = port.statistics();
  Serial.printf("{\"type\":\"status\",\"uart\":%u,\"mode\":\"%s\",\"active\":%s,\"error\":%u,\"slots\":%u,\"rxFrames\":%lu,"
                "\"rxErrors\":%lu,\"txFrames\":%lu,\"txTimeouts\":%lu,\"freeHeap\":%lu,"
                "\"load\":\"%s\",\"busyUs\":[%lu,%lu],\"loadCycles\":[%lu,%lu],"
                "\"wifiMode\":%u,\"scansStarted\":%lu,\"scansCompleted\":%lu,\"scanFailures\":%lu}\n",
                static_cast<unsigned>(NOCTE_HIL_UART_NUMBER),
                mode, port.isActive() ? "true" : "false", static_cast<unsigned>(port.lastError()),
                port.numberOfSlots(), static_cast<unsigned long>(stats.receivedFrames),
                static_cast<unsigned long>(stats.receiveErrors),
                static_cast<unsigned long>(stats.transmittedFrames),
                static_cast<unsigned long>(stats.transmitTimeouts),
                static_cast<unsigned long>(ESP.getFreeHeap()), loadMode,
                static_cast<unsigned long>(busyUs[0].load()), static_cast<unsigned long>(busyUs[1].load()),
                static_cast<unsigned long>(loadCycles[0].load()), static_cast<unsigned long>(loadCycles[1].load()),
                static_cast<unsigned>(WiFi.getMode()), static_cast<unsigned long>(scansStarted),
                static_cast<unsigned long>(scansCompleted), static_cast<unsigned long>(scanFailures));
}

void execute() {
  if (strncmp(command, "load ", 5) == 0) {
    if (!setLoad(command + 5)) Serial.println("Invalid load profile or load setup failed");
    status();
    return;
  }
  if (strncmp(command, "branch ", 7) == 0) {
    char* end = nullptr;
    const uint64_t low = strtoull(command + 7, &end, 16);
    const uint64_t high = strtoull(end, nullptr, 16);
    nocte::dmx::core::Uid found;
    const auto result = port.discoverRdmBranch(nocte::dmx::core::Uid(low),
        nocte::dmx::core::Uid(high), &found);
    char uid[14];
    found.format(uid, sizeof(uid));
    const auto timing = port.rdmReceiveTiming();
    Serial.printf("{\"type\":\"discovery\",\"result\":%u,\"uid\":\"%s\",\"spacingUs\":%lu,\"packetUs\":%lu,\"length\":%u}\n",
        static_cast<unsigned>(result), uid,
        static_cast<unsigned long>(timing.responseSpacingUs),
        static_cast<unsigned long>(timing.packetUs), port.copyRdmResponse(response, sizeof(response)));
    return;
  }
  if (strncmp(command, "mute ", 5) == 0 || strncmp(command, "unmute ", 7) == 0) {
    const bool mute = command[0] == 'm';
    const nocte::dmx::core::Uid target(strtoull(command + (mute ? 5 : 7), nullptr, 16));
    const auto result = port.setRdmDiscoveryMute(target, mute);
    Serial.printf("{\"type\":\"mute\",\"status\":%u,\"pdl\":%u,\"flags\":%u}\n",
        static_cast<unsigned>(result.status), result.parameterDataLength, result.validationFailures);
    return;
  }
  if (strncmp(command, "scan", 4) == 0) {
    nocte::dmx::core::DeviceTable<16> devices;
    const unsigned limit = command[4] == ' ' ? strtoul(command + 5, nullptr, 10) : 512;
    const uint32_t before = ESP.getFreeHeap();
    const auto result = nocte::dmx::core::scanRdmDevices(port, devices,
        static_cast<uint16_t>(limit > 65535 ? 65535 : limit));
    Serial.printf("{\"type\":\"scan\",\"status\":%u,\"transactions\":%u,\"unresolved\":%u,\"beforeHeap\":%lu,\"afterHeap\":%lu,\"uids\":[",
        static_cast<unsigned>(result.status), result.transactions, result.unresolved,
        static_cast<unsigned long>(before), static_cast<unsigned long>(ESP.getFreeHeap()));
    for (size_t i = 0; i < devices.count(); ++i) {
      nocte::dmx::core::Uid found;
      char uid[14];
      devices.get(i, found); found.format(uid, sizeof(uid));
      Serial.printf(i ? ",\"%s\"" : "\"%s\"", uid);
    }
    Serial.println("]}");
    return;
  }
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
    Serial.println("Commands: input / output / output24 / rdm / get HEX_PID [capacity] / set HEX_PID HEX_DATA / stop / status / frame / quiet / verbose / load none|cpu|wifi|combined");
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
  Serial.printf("NocteDMX S3 UART%u bench: TX=17 RX=18, input mode, no RE/DE\n",
                static_cast<unsigned>(NOCTE_HIL_UART_NUMBER));
}

void loop() {
  serviceWifiLoad();
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
