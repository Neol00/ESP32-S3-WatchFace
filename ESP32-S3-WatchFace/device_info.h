/* ============================================================================
 *  device_info.h — Central product identity (name, version, author, radio name).
 *
 *  Single source of truth for what this firmware calls itself. Surfaced in the
 *  boot log and the Settings > About screen, and (later) used for the BLE
 *  advertising name and WiFi-AP SSID once provisioning lands.
 *
 *  Included early in WatchFace's main sketch so every module can see these.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

#define DEVICE_NAME    "ESP32-S3-WatchFace"   // full product name
#define DEVICE_SHORT   "WatchFace"            // short form for radio names
#define DEVICE_VERSION "1.1.0"                // firmware version
#define DEVICE_AUTHOR  "Noel Ejemyr"          // project author
#define DEVICE_VENDOR  "Waveshare"            // hardware vendor
#define DEVICE_BOARD   "ESP32-S3-Touch-AMOLED-2.06"  // board model

/* Phone-friendly radio name: "WatchFace-AB12" using the last two bytes of the
 * factory MAC, so multiple units stay distinct in a device list. Kept short to
 * fit BLE/SSID length limits. Use for the BLE advertising name / WiFi-AP SSID
 * once provisioning is implemented (unused for now). */
static inline String deviceRadioName() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X",
           (uint8_t)(mac >> 8), (uint8_t)mac);
  return String(DEVICE_SHORT "-") + suffix;
}
