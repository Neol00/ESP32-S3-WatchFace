#!/usr/bin/env bash
# Apply the ESP32-S3-WatchFace library patches (LVGL, Arduino_GFX, ESP32 core BLE).
#
# Dry-runs every patch first and aborts if ANY would not apply cleanly, so it never
# leaves your tree half-patched. Skips patches that are already applied.
#
# Usage:
#   ./apply_patches.sh [LIBRARIES_DIR] [ESP32_CORE_DIR]
#
#   LIBRARIES_DIR   your Arduino libraries folder (has lvgl/, lv_conf.h,
#                   GFX_Library_for_Arduino/).  Default: ~/Arduino/libraries
#   ESP32_CORE_DIR  installed ESP32 core root (has libraries/BLE).
#                   Default: ~/.arduino15/packages/esp32/hardware/esp32/3.3.8
#
# Requires `patch`.
set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBRARIES_DIR="${1:-$HOME/Arduino/libraries}"
ESP32_CORE_DIR="${2:-$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.8}"

LVGL_DIR="$LIBRARIES_DIR/lvgl"
GFX_DIR="$LIBRARIES_DIR/GFX_Library_for_Arduino"
BLE_DIR="$ESP32_CORE_DIR/libraries/BLE"
I2S_DIR="$ESP32_CORE_DIR/libraries/ESP_I2S"

command -v patch >/dev/null 2>&1 || { echo "ERROR: 'patch' not found. Install it (e.g. apt install patch)."; exit 1; }

# job: "patchfile|applyroot|description"
JOBS=(
  "01-lvgl-freertos-corepin.patch|$LVGL_DIR|LVGL render-thread core pin"
  "02-lv_conf-snapshot.patch|$LIBRARIES_DIR|lv_conf.h snapshot enable"
  "03-gfx-qspi-dma.patch|$GFX_DIR|GFX QSPI async-DMA flush"
  "04-gfx-qspi-header.patch|$GFX_DIR|GFX QSPI second transaction struct"
  "05-esp32-ble-gap.patch|$BLE_DIR|ESP32 core BLE GAP-listener unregister"
  "06-esp32-psram-size.patch|$ESP32_CORE_DIR|ESP32 core getPsramSize physical chip size"
  "08-esp32-i2s-channel-leak.patch|$I2S_DIR|ESP32 core ESP_I2S channel-leak fix"
)

for d in "$LVGL_DIR" "$LIBRARIES_DIR" "$GFX_DIR" "$BLE_DIR" "$I2S_DIR"; do
  [ -d "$d" ] || { echo "ERROR: target directory not found: $d"; echo "Check the LIBRARIES_DIR / ESP32_CORE_DIR arguments."; exit 1; }
done

echo "ESP32-S3-WatchFace — applying library patches"
echo "  libraries:  $LIBRARIES_DIR"
echo "  esp32 core: $ESP32_CORE_DIR"
echo

# state: echoes applies | applied | fails
# --fuzz=0 forbids loose context matching, so an ALREADY-applied patch correctly fails
# the forward check (otherwise a small hunk can fuzzily "re-apply" and double-patch).
patch_state() {
  local pf="$1" root="$2"
  if patch -p1 -d "$root" --dry-run --force --fuzz=0 <"$pf" >/dev/null 2>&1; then echo applies; return; fi
  if patch -p1 -R -d "$root" --dry-run --force --fuzz=0 <"$pf" >/dev/null 2>&1; then echo applied; return; fi
  echo fails
}

declare -a STATES
hard_fail=0
i=0
for job in "${JOBS[@]}"; do
  IFS='|' read -r pf root desc <<<"$job"
  pp="$PATCH_DIR/$pf"
  [ -f "$pp" ] || { echo "ERROR: missing patch file: $pp"; exit 1; }
  st="$(patch_state "$pp" "$root")"
  STATES[$i]="$st"
  case "$st" in
    applies) echo "  [will apply] $desc" ;;
    applied) echo "  [already ok] $desc" ;;
    fails)   echo "  [CANNOT APPLY] $desc  ($pf)"; hard_fail=1 ;;
  esac
  i=$((i+1))
done

if [ "$hard_fail" -ne 0 ]; then
  echo
  echo "Aborting: at least one patch does not apply cleanly. Nothing was changed."
  echo "Most likely your library version differs. Required: LVGL 9.5.0, Arduino_GFX 1.6.5, ESP32 core 3.3.8."
  exit 1
fi

echo
i=0
for job in "${JOBS[@]}"; do
  IFS='|' read -r pf root desc <<<"$job"
  if [ "${STATES[$i]}" = "applies" ]; then
    patch -p1 -d "$root" --fuzz=0 <"$PATCH_DIR/$pf" >/dev/null
    echo "  applied: $desc"
  fi
  i=$((i+1))
done

echo
echo "Done. Now clear the Arduino build cache and rebuild, e.g.:"
echo "  rm -rf ~/.cache/arduino/sketches/*"
