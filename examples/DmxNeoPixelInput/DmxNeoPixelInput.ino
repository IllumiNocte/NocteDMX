/*
 * NocteDMX - DMX to NeoPixel
 *
 * Maps DMX addresses 1..36 to twelve RGB pixels. Install the Adafruit
 * NeoPixel library before compiling this optional example.
 *
 * ESP8266 wiring:
 *   UART0 RX (GPIO3) -> RS-485 RO
 *   GPIO5           -> RS-485 DE and active-low /RE tied together
 *   GPIO14          -> NeoPixel data input
 */

#include <NocteDMX.h>
#include <Adafruit_NeoPixel.h>

namespace {

constexpr uint8_t kDirectionPin = 5;
constexpr uint8_t kPixelPin = 14;
constexpr uint16_t kPixelCount = 12;
constexpr uint16_t kChannelCount = kPixelCount * 3;

nocte::dmx::Port& dmx = nocte::dmx::defaultPort();
Adafruit_NeoPixel pixels(kPixelCount, kPixelPin, NEO_GRB + NEO_KHZ800);
volatile bool frameAvailable = false;
uint8_t channels[kChannelCount] = {};

void NOCTE_DMX_ISR_ATTR onDmxFrame(int slots) {
  (void)slots;
  frameAvailable = true;
}

uint8_t gammaCorrect(uint8_t value) {
  return static_cast<uint16_t>(value) * value / 255;
}

}  // namespace

void setup() {
  pixels.begin();
  pixels.clear();
  pixels.show();

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

  const uint16_t copied = dmx.copyFrame(channels, kChannelCount);
  const uint16_t completePixels = copied / 3;
  for (uint16_t pixel = 0; pixel < completePixels; ++pixel) {
    const uint16_t offset = pixel * 3;
    pixels.setPixelColor(
        pixel,
        gammaCorrect(channels[offset]),
        gammaCorrect(channels[offset + 1]),
        gammaCorrect(channels[offset + 2]));
  }
  pixels.show();
}
