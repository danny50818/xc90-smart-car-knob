#!/bin/zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /dev/cu.usbmodemXXXX"
  echo "Restores the backed-up factory app without changing bootloader, partitions or NVS."
  exit 2
fi

SCRIPT_DIR="${0:A:h}"
FIRMWARE="$SCRIPT_DIR/backups/original-factory-app-3MB-2026-10-15.bin"
ESPTOOL="/Users/liuken-hsiang/Library/Arduino15/packages/esp32/tools/esptool_py/4.6/esptool"

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Missing factory backup: $FIRMWARE"
  exit 1
fi

"$ESPTOOL" --chip esp32s3 --port "$1" --baud 115200 \
  write_flash 0x10000 "$FIRMWARE"
