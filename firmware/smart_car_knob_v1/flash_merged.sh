#!/bin/zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /dev/cu.usbmodemXXXX"
  echo "This first-install image replaces the whole flash, including old BLE bonds."
  exit 2
fi

SCRIPT_DIR="${0:A:h}"
FIRMWARE="$SCRIPT_DIR/build-idf51/smart_car_knob_v1.ino.merged.bin"
ESPTOOL="/Users/liuken-hsiang/Library/Arduino15/packages/esp32/tools/esptool_py/4.6/esptool"

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Missing firmware: $FIRMWARE"
  echo "Compile with --export-binaries first."
  exit 1
fi

"$ESPTOOL" --chip esp32s3 --port "$1" --baud 921600 \
  write_flash 0x0 "$FIRMWARE"
