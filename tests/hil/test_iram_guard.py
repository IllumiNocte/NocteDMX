import unittest
import check_s3_iram as guard


class IramGuardTests(unittest.TestCase):
    def test_esp8266_outlined_lock_destructor_rejected(self):
        table = "\n".join("40100100 g F .text1 00000100 " + s for s in guard.ESP8266_SYMBOLS)
        code = {s: "40100100: call0 40000600 <rom_delay>" for s in guard.ESP8266_SYMBOLS}
        guard.validate(table, code, "esp8266")
        code[guard.ESP8266_SYMBOLS[0]] += "\n40100104: l32r a0, 401001f8 (40204c68 <InterruptLockD2>)"
        with self.assertRaisesRegex(ValueError, "Flash dependency"):
            guard.validate(table, code, "esp8266")

    def good(self):
        table = "\n".join("40375400 g F .iram0.text 00000100 " + s for s in guard.SYMBOLS)
        code = {s: "40375400: call8 40377e60 <esp_timer_get_time>\n"
                   "40375403: l32r a8, 40374410 (40000600 <esp_rom_delay_us>)"
                for s in guard.SYMBOLS}
        return table, code

    def test_rom_and_iram_calls_allowed(self):
        guard.validate(*self.good())

    def test_flash_placement_rejected(self):
        table, code = self.good()
        with self.assertRaisesRegex(ValueError, "outside IRAM"):
            guard.validate(table.replace(".iram0.text", ".flash.text", 1), code)

    def test_outlined_atomic_flash_call_rejected(self):
        table, code = self.good()
        code[guard.SYMBOLS[0]] += "\n40375410: l32r a8, 40374414 (42095dfc <atomic_bool_load>)"
        with self.assertRaisesRegex(ValueError, "Flash dependency"):
            guard.validate(table, code)

    def test_flash_rodata_dependency_rejected(self):
        table, code = self.good()
        code[guard.SYMBOLS[0]] += "\n40375410: l32r a8, 40374414 (3c010000 <message>)"
        with self.assertRaisesRegex(ValueError, "Flash dependency"):
            guard.validate(table, code)

    def test_iram_literal_slot_does_not_hide_flash_target(self):
        table, code = self.good()
        code[guard.SYMBOLS[0]] += (
            "\n40375410: l32r a8, 40374414 <_iram_text_start+0x10> (42095dfc <atomic_bool_load>)")
        with self.assertRaisesRegex(ValueError, "Flash dependency"):
            guard.validate(table, code)


if __name__ == "__main__":
    unittest.main()
