/**************************************************************************/
/*!
    @file     UID.cpp
    @author   Claude Heintz
    @license  BSD (see LXESP32DMX.h)
    @copyright 2017 by Claude Heintz

    RDM support for DMX Driver for ESP32

    @section  HISTORY

    v1.0 - First release
*/
/**************************************************************************/

#include <Arduino.h>
#include <rdm/UID.h>
#include <Print.h>

UID::UID() = default;
UID::UID(uint64_t u) : nocte::dmx::core::Uid(u) {}
UID::UID(const uint8_t* address) : nocte::dmx::core::Uid(address) {}
UID::UID(uint8_t m1, uint8_t m2, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
  setBytes(m1, m2, d1, d2, d3, d4);
}
UID& UID::operator=(const uint8_t* address) {
  if (address) memcpy(data(), address, nocte::dmx::rdm::kUidSize);
  return *this;
}
UID& UID::operator=(const UID& address) {
  nocte::dmx::core::Uid::operator=(address);
  return *this;
}
bool UID::operator==(const UID& address) const {
  return nocte::dmx::core::Uid::operator==(address);
}
bool UID::operator==(const uint8_t* address) const {
  return address && memcmp(data(), address, nocte::dmx::rdm::kUidSize) == 0;
}
uint8_t UID::becomeMidpoint(const UID& a, const UID& b) {
  return setMidpoint(a, b);
}
uint8_t* UID::rawbytes() { return data(); }
const uint8_t* UID::rawbytes() const { return data(); }
void UID::setBytes(uint64_t u) { setValue(u); }
void UID::setBytes(const UID& u) { *this = u; }
void UID::setBytes(uint8_t m1, uint8_t m2, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
  const uint8_t bytes[] = {m1, m2, d1, d2, d3, d4};
  *this = bytes;
}
uint64_t UID::getValue() const { return value(); }
size_t UID::printTo(Print& p) const {
  char text[14];
  format(text, sizeof(text));
  return p.print(text);
}
String UID::toString() const {
  char text[14];
  format(text, sizeof(text));
  return String(text);
}
void print64Bit(uint64_t n) {
  UID uid(n);
  Serial.print(uid);
}
uint64_t uid_bytes2long(uint8_t* b) { return uid_bytes2long(static_cast<const uint8_t*>(b)); }
uint64_t uid_bytes2long(const uint8_t* b) { return nocte::dmx::core::Uid(b).value(); }
void uid_long2Bytes(uint64_t u, uint8_t* bytes) {
  const nocte::dmx::core::Uid uid(u);
  if (bytes) memcpy(bytes, uid.data(), nocte::dmx::rdm::kUidSize);
}
