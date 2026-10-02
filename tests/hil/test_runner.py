"""Host-runner dry tests: mock serial/programming; never contact hardware."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock


def load_runner():
    spec = importlib.util.spec_from_file_location("nocte_hil_runner", Path(__file__).with_name("run_smoke.py"))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict("sys.modules", {"serial": mock.Mock()}):
        spec.loader.exec_module(module)
    return module


class RunnerDryTests(unittest.TestCase):
    def setUp(self):
        self.runner = load_runner()

    def check_image(self, chip, suffix, executable=None, uart=None, variant=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.runner.__file__ = str(root / "tests" / "hil" / "run_smoke.py")
            args = SimpleNamespace(chip=chip, arduino_cli="arduino-cli", fqbn="test-board",
                                   direction_pin=255, esp_port="mock-port",
                                   esptool_script=None, serial_module_path=None,
                                   esptool_executable=executable)
            if uart is not None:
                args.uart = uart
            if variant:
                args.build_variant = variant
                args.extra_build_flags = "-DNOCTE_HIL_RDM=0"

            def compile_only(command, check):
                self.assertTrue(check)
                if command[0] == "arduino-cli":
                    build = Path(command[command.index("--build-path") + 1])
                    build.mkdir(parents=True)
                    (build / ("DmxInputSmoke" + suffix)).write_bytes(b"mock image")

            fixture = mock.Mock()
            with mock.patch.object(self.runner.subprocess, "run", side_effect=compile_only) as run:
                self.runner.build_and_flash(args, fixture, "extras/hil/DmxInputSmoke")
            fixture.idle.assert_called_once()
            self.assertEqual(run.call_count, 2)
            compile_command = run.call_args_list[0].args[0]
            flags = compile_command[compile_command.index("--build-property") + 1]
            self.assertTrue(flags.startswith("compiler.cpp.extra_flags=-DNOCTE_HIL_DIRECTION_PIN=255"))
            if variant:
                self.assertIn("-DNOCTE_HIL_RDM=0", flags)
                self.assertEqual(Path(compile_command[compile_command.index("--build-path") + 1]).name,
                                 variant)
            if uart is not None:
                self.assertIn("-DNOCTE_HIL_UART_NUMBER=" + str(uart), flags)
                self.assertEqual(Path(compile_command[compile_command.index("--build-path") + 1]).name,
                                 "uart" + str(uart))
            self.assertFalse(any(flag.startswith("build.extra_flags=") for flag in compile_command))
            flash_command = run.call_args_list[1].args[0]
            if executable:
                self.assertEqual(flash_command[0], executable)
                self.assertNotIn("-m", flash_command)
            self.assertEqual(flash_command[flash_command.index("--chip") + 1], chip)
            self.assertEqual(flash_command[-2], "0x0")
            self.assertTrue(flash_command[-1].endswith(suffix))

    def test_esp8266_application_image(self):
        self.check_image("esp8266", ".ino.bin")

    def test_esp8266_output_variant_is_separate_and_flagged(self):
        self.check_image("esp8266", ".ino.bin", variant="output")

    def test_s3_merged_image(self):
        self.check_image("esp32s3", ".ino.merged.bin")

    def test_s3_standalone_esptool(self):
        self.check_image("esp32s3", ".ino.merged.bin", "vendor-esptool.exe")

    def test_s3_uart2_build_is_separate_and_flagged(self):
        self.check_image("esp32s3", ".ino.merged.bin", "vendor-esptool.exe", uart=2)

    def test_s3_uart1_build_is_separate_and_flagged(self):
        self.check_image("esp32s3", ".ino.merged.bin", uart=1)

    def test_s3_missing_merged_image_does_not_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            self.runner.__file__ = str(Path(directory) / "tests" / "hil" / "run_smoke.py")
            args = SimpleNamespace(chip="esp32s3", arduino_cli="arduino-cli", fqbn="test-board",
                                   direction_pin=255, esp_port="mock-port",
                                   esptool_script=None, serial_module_path=None)
            with mock.patch.object(self.runner.subprocess, "run") as run:
                with self.assertRaises(FileNotFoundError):
                    self.runner.build_and_flash(args, mock.Mock(), "extras/hil/DmxInputSmoke")
            self.assertEqual(run.call_count, 1)  # Compile only; no programming command.

    def test_s3_rdm_is_rejected_before_serial_open(self):
        for group in ("all", "rdm"):
            with self.subTest(group=group):
                argv = ["run_smoke.py", "--esp-port", "a", "--fixture-port", "b",
                        "--chip", "esp32s3", "--tests", group]
                with mock.patch.object(self.runner.sys, "argv", argv), mock.patch("sys.stderr"):
                    with self.assertRaises(SystemExit) as result:
                        self.runner.main()
                self.assertEqual(result.exception.code, 2)
        self.runner.serial.Serial.assert_not_called()


if __name__ == "__main__":
    unittest.main()
