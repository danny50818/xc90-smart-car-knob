#!/bin/zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /dev/cu.usbmodemXXXX"
  echo "Writes only the app partition at 0x10000 and preserves factory NVS."
  exit 2
fi

SCRIPT_DIR="${0:A:h}"
FIRMWARE="$SCRIPT_DIR/build-idf51/smart_car_knob_v1.ino.bin"
ESPTOOL="/Users/liuken-hsiang/Library/Arduino15/packages/esp32/tools/esptool_py/4.6/esptool"

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Missing firmware: $FIRMWARE"
  echo "Compile with --export-binaries first."
  exit 1
fi

"$ESPTOOL" --chip esp32s3 --port "$1" --baud 115200 \
  write_flash 0x10000 "$FIRMWARE"
