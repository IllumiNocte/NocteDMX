/**************************************************************************/
/*!
    @file     TOD.cpp
    @author   Claude Heintz
    @license  BSD (see LXESP32DMX.h)
    @copyright 2017 by Claude Heintz

    RDM support for DMX Driver for ESP32

    @section  HISTORY

    v1.0 - First release
*/
/**************************************************************************/

#include <Arduino.h>
#include <rdm/TOD.h>

TOD::TOD() = default;
uint8_t TOD::addUID(const UID& uid) { return table.append(uid); }
uint8_t TOD::add(const UID& uid) { return table.add(uid); }
void TOD::removeUIDAt(int index) {
  if (index >= 0 && index % 6 == 0) table.remove(static_cast<size_t>(index / 6));
}
uint8_t TOD::getUIDAt(int index, UID* uid) {
  if (index < 0 || index % 6 != 0 || !uid) return 0;
  nocte::dmx::core::Uid value;
  if (!table.get(static_cast<size_t>(index / 6), value)) return 0;
  *uid = value.data();
  return 1;
}
int TOD::getNextUID(int index, UID* uid) {
  return getUIDAt(index, uid) ? index + 6 : -1;
}
void TOD::push(const UID& uid) { table.append(uid); }
uint8_t TOD::pop(UID* uid) {
  if (!uid) return 0;
  nocte::dmx::core::Uid value;
  if (!table.pop(value)) return 0;
  *uid = value.data();
  return 1;
}
uint8_t TOD::contains(const UID& uid) { return table.contains(uid); }
uint8_t TOD::count() { return static_cast<uint8_t>(table.count()); }
void TOD::reset() { table.clear(); }
uint8_t* TOD::rawBytes() { return table.data(); }
void TOD::printTOD() {
  Serial.print("TOD-");
  Serial.println(count());
  UID uid;
  for (int index = getNextUID(0, &uid); index >= 0; index = getNextUID(index, &uid)) {
    Serial.println(uid);
  }
}
