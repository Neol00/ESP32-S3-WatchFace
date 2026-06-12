# Library patches

This firmware needs **7 small modifications** to three third-party libraries. They live
outside the sketch (in your Arduino `libraries/` folder and the ESP32 core), so a fresh
install won't have them — you must apply these patches once, or the build will fail /
the watch will misbehave (crash on BLE toggle, render on one core, no screen cache,
speaker that dies after a few chimes).

All changes are clearly marked in-source with a `LOCAL PATCH (ESP32-S3-WatchFace)`
comment, and each is GPL-compatible (the upstream licenses are MIT / Apache-2.0 /
LGPL-2.1).

## What each patch does

| # | Patch | Library (version) | License | Effect |
|---|-------|-------------------|---------|--------|
| 01 | `01-lvgl-freertos-corepin.patch` | LVGL **9.5.0** | MIT | Pins LVGL's SW render thread to core 0 so the 2nd draw unit uses the idle core (dual-core rendering). |
| 02 | `02-lv_conf-snapshot.patch` | LVGL **9.5.0** config | MIT | Enables `LV_USE_SNAPSHOT` so the PSRAM screen cache can pre-render screens. |
| 03 | `03-gfx-qspi-dma.patch` | Arduino_GFX **1.6.5** | MIT | Pipelined (async-DMA) QSPI flush: overlaps pixel-format conversion with transmission for higher FPS. |
| 04 | `04-gfx-qspi-header.patch` | Arduino_GFX **1.6.5** | MIT | Adds the second SPI transaction struct the pipelined flush needs. |
| 05 | `05-esp32-ble-gap.patch` | ESP32 Arduino core **3.3.8** | Apache-2.0 | Unregisters the custom GAP event listener in `BLEDevice::deinit()` — fixes a crash when BLE is toggled off then on again. |
| 06 | `06-esp32-psram-size.patch` | ESP32 Arduino core **3.3.8** | LGPL-2.1 | `ESP.getPsramSize()` reports the PHYSICAL chip size (8 MB) instead of the heap total, which read "6 MB" once XIP-from-PSRAM (required by 120 MHz DDR PSRAM) reserved ~2 MB for app code before heap init. |
| 08 | `08-esp32-i2s-channel-leak.patch` | ESP32 Arduino core **3.3.8** | LGPL-2.1 | `I2SClass::end()` no longer aborts before `i2s_del_channel()` when disable errors, and `begin()` recycles still-allocated channels instead of overwriting the handles — fixes the speaker dying with `i2s_new_channel: no available channel found` after a teardown raced the render task. |
| 07 | `07-libbuilder-esp32s3.patch` | [esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder) @ `43a8f6d` | Apache-2.0 | **Not applied to your Arduino install** — documents how the custom `esp32s3-libs` package (below) was built: NimBLE host allocations → PSRAM, IDF libs compiled `-O2` instead of `-Os`, XIP-from-PSRAM + 120 MHz DDR PSRAM enablement, FreeRTOS run-time stats for real per-core CPU usage, PSRAM task stacks, S3-only build matrix. |

> **Versions matter.** These patches were generated against the exact versions above.
> If your libraries differ, a patch may not apply cleanly — install the matching
> versions (LVGL 9.5.0, Arduino_GFX 1.6.5, ESP32 core 3.3.8), or apply the change by
> hand using the `LOCAL PATCH` markers as a guide.

## Where the files live

- **LVGL** (`lvgl/`) and its `lv_conf.h` — in your Arduino `libraries/` folder
  (typically `Documents/Arduino/libraries/`). `lv_conf.h` sits **next to** the `lvgl`
  folder, not inside it.
- **Arduino_GFX** (`GFX_Library_for_Arduino/`) — same `libraries/` folder.
- **ESP32 core** — the installed core at
  `…/Arduino15/packages/esp32/hardware/esp32/3.3.8/`: its bundled `libraries/BLE/`
  (patch 05) and `libraries/ESP_I2S/` (patch 08), plus `cores/esp32/Esp.cpp`
  (patch 06).

## How to apply

### Automatic (recommended)

From this `patches/` directory, run the script for your OS and point it at your
Arduino `libraries/` folder and your ESP32 core folder:

**Windows (PowerShell):**
```powershell
./apply_patches.ps1 -LibrariesDir "$HOME\Documents\Arduino\libraries" `
                    -Esp32CoreDir "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.3.8"
```

**Linux / macOS (bash, needs `patch`):**
```bash
./apply_patches.sh ~/Arduino/libraries \
  ~/.arduino15/packages/esp32/hardware/esp32/3.3.8
```

Both scripts **dry-run first** and refuse to apply if any patch wouldn't apply cleanly,
so they won't half-patch your tree. They're also idempotent-aware: if a patch is already
applied they tell you and skip it.

### Manual

From each library's root, apply with `patch -p1`:
```bash
cd <libraries>/lvgl                       && patch -p1 < 01-lvgl-freertos-corepin.patch
cd <libraries>                            && patch -p1 < 02-lv_conf-snapshot.patch   # lv_conf.h is here
cd <libraries>/GFX_Library_for_Arduino    && patch -p1 < 03-gfx-qspi-dma.patch
cd <libraries>/GFX_Library_for_Arduino    && patch -p1 < 04-gfx-qspi-header.patch
cd <esp32-core>/libraries/BLE             && patch -p1 < 05-esp32-ble-gap.patch
cd <esp32-core>                           && patch -p1 < 06-esp32-psram-size.patch
cd <esp32-core>/libraries/ESP_I2S         && patch -p1 < 08-esp32-i2s-channel-leak.patch
```

## The custom `esp32s3-libs` package (precompiled IDF libraries)

Besides the core source patches above, this firmware runs on a **self-compiled set of
precompiled ESP-IDF libraries** replacing the stock package at
`…/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.8/`. It cannot be distributed as a
patch: the package is ~175 static libraries (binaries) plus the headers/linker scripts
of the exact IDF snapshot it was built from — a wholesale replacement, not a diff.

- **Install:** download the `esp32s3-libs` zip from this project's releases and replace
  the entire `…/tools/esp32s3-libs/3.3.8/` folder with its contents (keep a backup of
  the stock folder if you want to revert).
- **Rebuild from source:** clone
  [esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder)
  at commit `43a8f6d`, apply `07-libbuilder-esp32s3.patch` (`git apply`), then run
  `./build.sh -t esp32s3` (Linux/WSL). The patch contains inline comments explaining
  every configuration change.
- **What it changes:** BLE (NimBLE) host memory moved to PSRAM (frees ~25-40 KB of
  SRAM — breaks BLE on boards *without* PSRAM!), IDF libraries compiled for speed
  (`-O2`) instead of size, XIP-from-PSRAM + 120 MHz DDR PSRAM support, accurate
  per-task CPU statistics, and FreeRTOS task stacks allowed in PSRAM.

## After applying

Clear the Arduino build cache so the patched libraries recompile, then rebuild:
- **Windows:** delete `%LOCALAPPDATA%\arduino\sketches\*`
- **Linux/macOS:** delete `~/.cache/arduino/sketches/*` (path varies by IDE version)
