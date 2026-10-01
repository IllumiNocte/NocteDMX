# Standalone hardware smoke tests

These tests use NocteDMX directly. They do not need uNode, Wi-Fi, a Raspberry
Pi host, Node-RED or a web API. GitHub CI compiles the test firmware, but the
physical tests run locally.
Firmware sketches live in `extras/hil`, as required by the Arduino library
layout. The host runner and its dependencies remain in `tests/hil`.

## Hardware

- ESP8266 with 4 MB flash and USB serial programming.
- RP2040/RP2350 with the uNode DMX tester firmware, tested with version 0.4.14.
- One RS485 transceiver per device, connected through a terminated DMX bus.
- ESP UART0 TX GPIO1 to DI, RX GPIO3 to RO, GPIO5 to tied DE and active-low /RE.
- RP2040 tester wiring follows its firmware's pin configuration.

Use only the synthetic tester responder. The test firmware changes start
address 42 to 43 and restores 42, then switches Identify on and off. It also
flashes the ESP and replaces the running firmware. Back up existing firmware
before the first run. The runner always puts the RP2040 in `idle` before
flashing: an enabled RS485 receiver can interfere with the ESP bootloader UART.
Cleanup returns the tester to the conformant profile and `idle`.

## Run

Install Arduino CLI, ESP8266 Arduino Core 3.1.2, and the Python dependencies:

```sh
python -m pip install -r tests/hil/requirements.txt
python tests/hil/run_smoke.py --esp-port COM12 --fixture-port COM3
```

Linux serial paths work as well. `--arduino-cli` accepts the full executable
path, `--direction-pin` changes tied DE and /RE, and `--tests output|input|rdm`
runs one group. `--report` selects the generated JSON report. Build artifacts
default to `build/hil`; dependencies are not installed by the runner.

The isolated Python bundled with Arduino may require a launcher that adds the
core's `tools/pyserial` directory to `sys.path`. For this installation, pass
`--esptool-script` and `--serial-module-path` for its bundled esptool child.
An ordinary Python installation with the requirements above needs neither.

## Coverage and limits

### ESP32-S3 preparation

An experimental DMX-only path is prepared with
`--chip esp32s3 --tests dmx` (or `output`/`input`). The default S3 build uses
UART1 TX17/RX18 and direction pin 255 (disabled). It compiles for Arduino Core
3.3.12 with USB CDC on boot and flashes the **merged** image at offset 0.
Use esptool 4.x/5.x. `all` and `rdm` are rejected for S3 before opening ports.
This path has not yet been run on S3 hardware. See [the bring-up guide](../../docs/esp32s3.md)
for crossed 3.3-V UART wiring and the dedicated interactive bench sketch.
The ESP8266 tests/defaults remain unchanged.
Host-runner dry tests mock all serial/programming calls and verify image
selection and the S3 RDM guard without Python hardware dependencies:

```sh
python -m unittest discover -s tests/hil -p 'test_*.py' -v
```

### ESP8266 tested scope

- 24-slot DMX output: two identical changing fade channels and zero elsewhere.
- 512-slot DMX input: every received channel is echoed and compared, including 512.
- Constructed port and UART ownership, including an inactive second port destructor.
- Per-port controller UID, full-range discovery and Mute/Unmute.
- DEVICE_INFO with reported PDL and guard bytes beyond the real payload.
- GET and SET start address, read-back, restore, Identify and NACK result.
- Oversized SET rejected before accessing its short input buffer or sending.
- Correct-checksum wrong-transaction replies rejected, then clean recovery.
- Corrupt discovery checksum rejected, then clean recovery.
- Observed RDM request Break/MAB and post-response controller spacing.

The RP2040 analyzer's edge flags and USB status snapshots are not a lossless
logic-analyzer capture. At the output example's ~767 Hz rate it can occasionally
merge frame edges. Output timing therefore requires five intact snapshots and
records all attempted snapshots. RDM statistics include both normal DMX and RDM
traffic; `shortFrames` does not mean an invalid RDM frame. These are regression
smoke tests, not a complete DMX512-A/E1.20 certification or worst-case load test.
Sustained USB JSON serialization can also disrupt the tester's synthetic RDM
responder. RDM checks therefore use short snapshots with 3.5-second quiet
windows. A successful cycle proves the functional assertions, not zero loss
under console load; that fixture limitation needs separate stress coverage.

RDM responder mode on the ESP, split DE//RE wiring, multi-responder collisions,
all timing boundary profiles and prolonged Wi-Fi load remain additional HIL
coverage. They are not claimed by a green run of this script.
