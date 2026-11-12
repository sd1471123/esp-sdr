#!/usr/bin/env bash
# Flash the ESP32-C5 panel firmware.
#
# Default: write bootloader + partition table + app, leaving the NVS partition
# (0x9000-0xF000) untouched, so mute/band/mode/四套量程 all survive the update.
#
# --erase-settings: write the merged 0x0 image instead. That fills the NVS
# window with 0xFF and therefore resets every saved setting. Use it for a first
# flash, after a partition-table change, or when a setting got corrupted.
#
# Environment: PORT (default COM6), BAUD (default 460800), PY (python with esptool).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-COM6}"
BAUD="${BAUD:-460800}"
PY="${PY:-python}"
FW="$HERE/firmware"

COMMON=(--chip esp32c5 -p "$PORT" -b "$BAUD"
        --before default-reset --after hard-reset
        write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m)

if [ "${1:-}" = "--erase-settings" ]; then
    echo "flashing merged image: NVS will be erased"
    "$PY" -m esptool "${COMMON[@]}" 0x0 "$FW/esp32c5-lcd-full.bin"
else
    echo "flashing app only: NVS (settings) preserved"
    "$PY" -m esptool "${COMMON[@]}" \
        0x2000 "$FW/esp32c5-lcd-bootloader.bin" \
        0x8000 "$FW/esp32c5-lcd-partition-table.bin" \
        0x10000 "$FW/esp32c5-lcd-app.bin"
fi
