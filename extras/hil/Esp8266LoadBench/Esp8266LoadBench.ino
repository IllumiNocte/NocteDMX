// Standalone ESP8266 UART0 load bench. Telemetry is DMX, not USB/Serial.
// Never connect to a show: RDM Identify is changed then restored each cycle.
#include <NocteDMX.h>
#include <ESP8266WiFi.h>
#include <rdm/UID.h>
#include <rdm/rdm_utility.h>

#ifndef NOCTE_HIL_DIRECTION_PIN
#define NOCTE_HIL_DIRECTION_PIN 255
#endif
#ifndef NOCTE_HIL_RDM
#define NOCTE_HIL_RDM 1
#endif

namespace {
nocte::dmx::Port port;
uint8_t frame[512] = {};
const UID fixtureUid(UINT64_C(0x7FF052444D01));
volatile uint32_t irqCount = 0;
uint32_t irqCycles = 0, scansCompleted = 0, scanFailures = 0, cpuWindows = 0;
uint32_t goodGets = 0, badGets = 0, scanAttempts = 0, badScans = 0, badLongRequests = 0;
uint32_t epoch = 0;
uint16_t cycle = 0;
uint8_t profile = 255;
uint8_t accumulatedFailures = 0;
bool scanning = false;
volatile uint32_t cpuSink = 0;

void IRAM_ATTR loadInterrupt() {
  const uint32_t started = ESP.getCycleCount();
  while (static_cast<uint32_t>(ESP.getCycleCount() - started) < irqCycles) {}
  ++irqCount;
}

void setLoad(uint8_t next) {
  timer1_disable();
  timer1_detachInterrupt();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  scanning = false;
  profile = next;
  if (profile & 2) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("NocteDMX-8266-HIL", "nocte-hil-load");
  }
  if (profile & 1) {
    timer1_attachInterrupt(loadInterrupt);
    timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
    timer1_write(5000); // 80-MHz APB /16: one interrupt per millisecond.
  }
}

void serviceLoad() {
  if (profile & 2) {
    const int result = WiFi.scanComplete();
    if (scanning && result >= 0) {
      ++scansCompleted;
      WiFi.scanDelete();
      scanning = false;
    } else if (scanning && result == WIFI_SCAN_FAILED) {
      ++scanFailures;
      scanning = false;
    }
    if (!scanning) {
      WiFi.scanNetworks(true);
      scanning = true;
    }
  }
  if (profile & 1) {
    const uint32_t started = micros();
    uint32_t value = cpuSink;
    while (static_cast<uint32_t>(micros() - started) < 7000)
      value = value * 1664525u + 1013904223u;
    cpuSink = value;
    ++cpuWindows;
  }
}

void put32(uint8_t index, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) frame[index + i] = value >> (24 - 8 * i);
}

uint8_t exerciseRdm() {
  uint8_t failures = 0;
  if (cycle % 5 == 0) {
    UID lower(UINT64_C(0)), upper(UINT64_C(0xFFFFFFFFFFFF)), found;
    port.sendRDMDiscoveryMute(BROADCAST_ALL_DEVICES_ID, RDM_DISC_UNMUTE);
    ++scanAttempts;
    const uint8_t result = port.sendRDMDiscoveryPacket(lower, upper, &found);
    if (result != RDM_DID_DISCOVER || memcmp(found.rawbytes(), fixtureUid.rawbytes(), 6)
        || !port.sendRDMDiscoveryMute(fixtureUid, RDM_DISC_MUTE)) {
      failures |= 1;
      ++badScans;
    }
  }
  uint8_t info[25];
  memset(info, 0xA5, sizeof(info));
  auto read = port.getRdmParameter(fixtureUid, RDM_DEVICE_INFO, info, sizeof(info));
  const bool good = read.ok() && read.parameterDataLength == 19 && read.copiedLength == 19
      && info[19] == 0xA5 && info[24] == 0xA5;
  if (good) ++goodGets; else { ++badGets; failures |= 2; }
  for (uint8_t value : {1, 0}) {
    if (!port.setRdmParameter(fixtureUid, RDM_IDENTIFY_DEVICE, &value, 1).ok()) failures |= 4;
    uint8_t actual = 0xA5;
    read = port.getRdmParameter(fixtureUid, RDM_IDENTIFY_DEVICE, &actual, 1);
    if (!read.ok() || read.parameterDataLength != 1 || actual != value) failures |= 8;
  }
  uint8_t maximum[231];
  memset(maximum, 0xFF, sizeof(maximum));
  const auto longSet = port.setRdmParameter(fixtureUid, 0x7FFE, maximum, sizeof(maximum));
  if (longSet.status != nocte::dmx::core::RdmCommandStatus::Nack) {
    ++badLongRequests;
    failures |= 16;
  }
  return failures;
}
}

void setup() {
  // Initialize then release the core's UART console before handing UART0 to
  // DMX. This bench also exercises early Wi-Fi initialization.
  Serial.begin(74880);
#ifdef NOCTE_HIL_STARTUP_TRACE
  Serial.println("HIL: setup"); Serial.flush();
#endif
  WiFi.persistent(false); // Load cycling must not rewrite user flash settings.
  WiFi.setAutoReconnect(false);
  irqCycles = ESP.getCpuFreqMHz() * 100; // 100-us maskable timer ISR.
  setLoad(2); // Warm radio storage before the first no-load baseline.
#ifdef NOCTE_HIL_STARTUP_TRACE
  Serial.println("HIL: radio warm"); Serial.flush();
#endif
  delay(1000);
  setLoad(0);
#ifdef NOCTE_HIL_STARTUP_TRACE
  Serial.println("HIL: radio off"); Serial.flush();
#endif
  Serial.end();
  port.setUid(nocte::dmx::core::Uid(UINT64_C(0x7FF000000002)));
#if NOCTE_HIL_RDM
  port.startRDM(NOCTE_HIL_DIRECTION_PIN, nocte::dmx::PortMode::Send);
#else
  port.setDirectionPin(NOCTE_HIL_DIRECTION_PIN);
  port.startOutput();
#endif
  epoch = millis();
}

void loop() {
  const uint32_t elapsed = millis() - epoch;
  // One finite sweep; never leave an unattended board in WLAN/IRQ stress.
  const uint8_t next = elapsed >= 80000 ? 0 : elapsed / 20000;
  if (next != profile) setLoad(next);
  ++cycle;
  const uint8_t failures = NOCTE_HIL_RDM && elapsed < 80000 ? exerciseRdm() : 0;
  accumulatedFailures |= failures; // Do not hide a failed SET between snapshots.
  serviceLoad();
  memset(frame, 0, sizeof(frame));
  frame[0] = 0xA5; frame[1] = 0xD6; frame[2] = 1; frame[3] = profile;
  frame[4] = NOCTE_HIL_RDM; frame[5] = port.isActive();
  frame[6] = cycle >> 8; frame[7] = cycle; frame[8] = accumulatedFailures;
  frame[9] = WiFi.getMode(); frame[10] = scanning;
  frame[11] = ESP.getCpuFreqMHz();
  put32(12, ESP.getFreeHeap());
  put32(16, port.rdmTransmitTimeoutCount());
  put32(20, irqCount); put32(24, scansCompleted); put32(28, cpuWindows);
  put32(32, goodGets); put32(36, badGets); put32(40, scanAttempts);
  put32(44, badScans); put32(48, badLongRequests);
  put32(52, port.lastRDMResponseFirstSlotDelayUs());
  put32(56, port.lastRDMResponseMaxSlotIntervalUs());
  put32(60, scanFailures);
  frame[511] = 0x5A;
  port.setFrame(frame, 512);
  delay(NOCTE_HIL_RDM ? 500 : 3);
}
