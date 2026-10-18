#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="${0:A:h}"
FIRMWARE="$SCRIPT_DIR/build/esp32.esp32.esp32s3/smart_car_knob_v1.ino.bin"
OPENOCD="/Users/liuken-hsiang/Library/Arduino15/packages/esp32/tools/openocd-esp32/v0.12.0-esp32-20260424/bin/openocd"
OPENOCD_SCRIPTS="/Users/liuken-hsiang/Library/Arduino15/packages/esp32/tools/openocd-esp32/v0.12.0-esp32-20260424/share/openocd/scripts"

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Missing firmware: $FIRMWARE"
  echo "Compile with --export-binaries first."
  exit 1
fi

"$OPENOCD" -s "$OPENOCD_SCRIPTS" -f board/esp32s3-builtin.cfg \
  -c "init; reset halt; program_esp $FIRMWARE 0x10000 verify reset exit"

