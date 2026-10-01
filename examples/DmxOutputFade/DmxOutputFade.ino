/*
 * NocteDMX - DMX output fade
 *
 * Sends a complete 24-channel DMX frame and fades channels 1 and 8. The
 * example uses only the public NocteDMX facade; the selected backend owns the
 * UART and interrupt implementation.
 *
 * ESP8266 wiring:
 *   UART0 TX (GPIO1) -> RS-485 DI
 *   GPIO5           -> RS-485 DE and active-low /RE tied together
 *
 * UART0 is occupied by DMX, so do not call Serial.begin().
 */

#include <NocteDMX.h>

#ifndef NOCTE_HIL_DIRECTION_PIN
#define NOCTE_HIL_DIRECTION_PIN 5
#endif

namespace {

constexpr uint8_t kDirectionPin = NOCTE_HIL_DIRECTION_PIN;
constexpr uint16_t kChannelCount = nocte::dmx::kMinimumOutputSlots;

nocte::dmx::Port& dmx = nocte::dmx::defaultPort();
uint8_t channels[kChannelCount] = {};
uint8_t level = 0;
int8_t step = 1;

}  // namespace

void setup() {
  dmx.setDirectionPin(kDirectionPin);
  dmx.setFrame(channels, kChannelCount);
  dmx.startOutput();
}

void loop() {
  channels[0] = level;  // DMX address 1
  channels[7] = level;  // DMX address 8
  dmx.setFrame(channels, kChannelCount);

  if (level == 0) {
    step = 1;
  } else if (level == 255) {
    step = -1;
  }
  level = static_cast<uint8_t>(level + step);
  delay(10);
}
