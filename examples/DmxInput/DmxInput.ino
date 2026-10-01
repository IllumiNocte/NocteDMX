/*
 * NocteDMX - DMX input
 *
 * Receives complete DMX frames and maps address 1 to a PWM LED. The receive
 * callback runs in interrupt context and therefore only sets a flag. Frame
 * data is copied atomically from loop().
 *
 * ESP8266 wiring:
 *   UART0 RX (GPIO3) -> RS-485 RO
 *   GPIO5           -> RS-485 DE and active-low /RE tied together
 *
 * UART0 is occupied by DMX, so do not call Serial.begin().
 */

#include <NocteDMX.h>

namespace {

constexpr uint8_t kDirectionPin = 5;
constexpr uint8_t kLedPin = 14;

nocte::dmx::Port& dmx = nocte::dmx::defaultPort();
volatile bool frameAvailable = false;

void NOCTE_DMX_ISR_ATTR onDmxFrame(int slots) {
  (void)slots;
  frameAvailable = true;
}

}  // namespace

void setup() {
  pinMode(kLedPin, OUTPUT);
  dmx.setDirectionPin(kDirectionPin);
  dmx.setDataReceivedCallback(onDmxFrame);
  dmx.startInput();
}

void loop() {
  if (!frameAvailable) {
    return;
  }

  noInterrupts();
  frameAvailable = false;
  interrupts();

  uint8_t level = 0;
  if (dmx.copyFrame(&level, 1) == 1) {
    analogWrite(kLedPin, static_cast<uint16_t>(level) * 4);
  }
}
