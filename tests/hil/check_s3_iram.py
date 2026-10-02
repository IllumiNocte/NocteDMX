"""Reject direct or literal-resolved flash dependencies in ESP timing windows.

This is a build guard, not a complete indirect-call graph verifier. The user
frame callback is an indirect call and must satisfy the documented ISR contract.
"""
import argparse
import re
import subprocess


PREFIX = "_ZN5nocte3dmx8backends15Esp32S3UartPort"
SYMBOLS = [PREFIX + suffix for suffix in (
    "12setDirectionEb", "13beginTransmitEv", "17finishRdmTransmitEv",
    "13uartInterruptEPv", "15rxEdgeInterruptEPv", "10fillTxFifoEv")]
SYMBOLS.append("_ZNK5nocte3dmx8backends15Esp32S3UartPort7rxLevelEv")


ESP8266_SYMBOLS = ["_ZN9LX8266DMX20beginRdmTransmissionEh",
                   "_ZN9LX8266DMX21finishRdmTransmissionEh",
                   "_ZN9LX8266DMX21setTransceiverReceiveEv",
                   "__digitalWrite", "micros", "delayMicroseconds"]


def validate(symbol_table, disassemblies, chip="esp32s3"):
    symbols = ESP8266_SYMBOLS if chip == "esp8266" else SYMBOLS
    section = ".text1" if chip == "esp8266" else ".iram0.text"
    for symbol in symbols:
        lines = [line for line in symbol_table.splitlines() if line.endswith(" " + symbol)]
        if len(lines) != 1 or section not in lines[0]:
            raise ValueError("Required IRAM symbol missing or placed outside IRAM: " + symbol)
        assembly = disassemblies[symbol]
        for line in assembly.splitlines():
            # Espressif Xtensa call8/callx8 addresses and literal loads display
            # their named ELF target in <...>. Ignore the section heading.
            # A literal instruction can name both its IRAM literal slot and
            # the flash function pointer stored there. Inspect every target.
            for match in re.finditer(r"(?:\(|\s)([0-9a-f]{8})\s+<[^>]+>", line):
                address = int(match.group(1), 16)
                flash = (0x40200000 <= address < 0x40300000) if chip == "esp8266" else (
                    0x42000000 <= address < 0x44000000 or 0x3C000000 <= address < 0x3E000000)
                if flash:
                    raise ValueError("Flash dependency in " + symbol + ": " + line.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--chip", choices=("esp32s3", "esp8266"), default="esp32s3")
    args = parser.parse_args()

    def dump(*arguments):
        return subprocess.check_output([args.objdump, *arguments, args.elf], text=True)

    symbols = ESP8266_SYMBOLS if args.chip == "esp8266" else SYMBOLS
    validate(dump("-t"), {symbol: dump("-d", "--disassemble=" + symbol) for symbol in symbols}, args.chip)
    print("PASS %s timing IRAM placement and resolved call targets (%d routines)" % (args.chip, len(symbols)))


if __name__ == "__main__":
    main()
