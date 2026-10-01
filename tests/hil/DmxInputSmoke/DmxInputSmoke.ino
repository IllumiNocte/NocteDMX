// Receive a known fixture pattern, then echo it as DMX after the sender stops.
#include <NocteDMX.h>

#ifndef NOCTE_HIL_DIRECTION_PIN
#define NOCTE_HIL_DIRECTION_PIN 5
#endif

namespace {
nocte::dmx::Port port;
volatile bool ready = false;
uint8_t frame[512] = {};
bool sent = false;
void NOCTE_DMX_ISR_ATTR received(int slots) { if (slots == 512) ready = true; }
}

void setup() {
  port.setDirectionPin(NOCTE_HIL_DIRECTION_PIN);
  port.setDataReceivedCallback(received);
  port.startInput();
}

void loop() {
  if (sent || !ready) { delay(1); return; }
  const uint16_t slots = port.copyFrame(frame, sizeof(frame));
  if (slots != 512) { ready = false; return; }
  port.stop();
  // Capture before the host stops TX: stopping mid-frame can otherwise replace
  // the last full frame with a valid shorter packet. Keep DE low until handoff.
  delay(2500);
  if (slots == 512) {
    port.setFrame(frame, slots);
    port.startOutput();
    sent = true;
  }
}
