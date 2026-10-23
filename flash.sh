#!/bin/sh
# Build both firmwares and flash them: Bluetooth mode -> app0 (boots first), WiFi mode -> app1.
# Usage: ./flash.sh [port]    (--no-build to flash existing binaries)
set -e
cd "$(dirname "$0")"
ARDUINO=~/.local/bin/arduino-cli
ESPTOOL=~/.venvs/esp/bin/esptool.py
FQBN=esp32:esp32:m5stack_core2
CORE=$(ls -d ~/Library/Arduino15/packages/esp32/hardware/esp32/* | tail -1)

if [ "$1" = "--no-build" ]; then shift; else
  $ARDUINO compile -b $FQBN --build-path build/bt-cache --output-dir build/bt . &
  $ARDUINO compile -b $FQBN --build-path build/wifi-cache --output-dir build/wifi \
    --build-property "compiler.cpp.extra_flags=-DCORE2_WIFI=1" . &
  wait %1 || { echo "Bluetooth build failed; not flashing"; exit 1; }
  wait %2 || { echo "WiFi build failed; not flashing"; exit 1; }
fi

PORT=${1:-$(ls /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "No Core2 serial port found"; exit 1; }

# Offsets from the default_16MB partition table.
$ESPTOOL --chip esp32 --port "$PORT" --baud 460800 write_flash -z \
  0x1000   build/bt/tiny-tunes.ino.bootloader.bin \
  0x8000   build/bt/tiny-tunes.ino.partitions.bin \
  0xe000   "$CORE/tools/partitions/boot_app0.bin" \
  0x10000  build/bt/tiny-tunes.ino.bin \
  0x650000 build/wifi/tiny-tunes.ino.bin
