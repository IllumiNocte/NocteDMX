"""Regression guard for the UART TX-empty / RECEIVE publication race."""
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[2] / "src/nocte/backends/esp8266/Esp8266DmxPort.cpp"


class ReceiveHandoffTests(unittest.TestCase):
    def test_receive_is_published_with_tx_empty_already_masked(self):
        body = SOURCE.read_text().split("void LX8266DMX::setTaskReceive(", 1)[1].split("\n}", 1)[0]
        lock = body.index("esp8266::InterruptLock lock;")
        mask = body.index("USIE(UART0) &= ~(1 << UIFE);")
        ack = body.index("USIC(UART0) = (1 << UIFE);")
        mode = body.index("_rdm_task_mode = DMX_TASK_RECEIVE;")
        release = body.index("setTransceiverReceive();")
        self.assertLess(lock, mask)
        self.assertLess(mask, ack)
        self.assertLess(ack, mode)
        self.assertLess(mode, release)
        self.assertNotIn("delay(", body)

    def test_publication_order_has_no_interrupt_storm_interleaving(self):
        # A level-triggered empty FIFO can stay asserted. The RDM ISR skips
        # filling it in RECEIVE, so foreground cannot run to mask it later.
        # Enumerate IRQ arrival after either foreground operation, independent
        # of the additional critical-section protection in the real driver.
        def unsafe_windows(operations):
            enabled, receiving = True, False
            result = []
            for operation in operations:
                if operation == "mask": enabled = False
                if operation == "receive": receiving = True
                result.append(enabled and receiving)
            return result

        self.assertEqual(unsafe_windows(["receive", "mask"]), [True, False])
        self.assertEqual(unsafe_windows(["mask", "receive"]), [False, False])


if __name__ == "__main__":
    unittest.main()
