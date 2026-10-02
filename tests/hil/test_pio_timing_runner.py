import unittest
from unittest import mock
with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
    import run_s3_pio_timing as runner


class PioTimingTests(unittest.TestCase):
    def reply(self):
        return dict(method="pio_dma_raw", valid=True, stalled=False, timedOut=False,
                    words=16384, sampleUs=0.25, shortMabs=0,
                    breakUs=dict(n=5, min=176), mabUs=dict(n=5, min=16))

    def test_good_capture(self):
        runner.verify_capture(self.reply())

    def test_old_fixture_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, "firmware"):
            runner.verify_capture({"method": "gpio_irq"})

    def test_stall_timeout_incomplete_wrong_rate_are_rejected(self):
        for field, value in (("stalled", True), ("timedOut", True), ("valid", False),
                             ("words", 1000), ("sampleUs", 0.5)):
            with self.subTest(field=field):
                reply = self.reply()
                reply[field] = value
                with self.assertRaises(AssertionError): runner.verify_capture(reply)

    def test_short_mab_is_rejected(self):
        reply = self.reply()
        reply["mabUs"]["min"] = 7
        reply["shortMabs"] = 1
        with self.assertRaisesRegex(AssertionError, "below DMX"):
            runner.verify_capture(reply)

    def test_no_signal_is_not_a_pass(self):
        reply = self.reply()
        reply["breakUs"]["n"] = 0
        with self.assertRaisesRegex(AssertionError, "Not enough"):
            runner.verify_capture(reply)


if __name__ == "__main__": unittest.main()
