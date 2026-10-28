#!/usr/bin/env bash
# Flash AOG Switch onto an ESP32-C3 Super Mini (whole flash image at 0x0; settings start fresh).
#   ./firmware/flash_aog_switch.sh [/dev/ttyACM0]
set -e
PORT="${1:-/dev/ttyACM0}"
ESPTOOL="$(ls -d ~/.arduino15/packages/esp32/tools/esptool_py/*/esptool | tail -1)"
"$ESPTOOL" --chip esp32c3 -p "$PORT" -b 921600 write-flash 0x0 "$(dirname "$0")/aog_switch_esp32c3_merged.bin"
