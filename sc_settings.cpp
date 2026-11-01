#include "sc_settings.h"
#include <string.h>
#include <nvs.h>
#include <nvs_flash.h>

#define SETTINGS_VERSION 1

Settings g_settings;

static void setDefaults() {
  memset(&g_settings, 0, sizeof(g_settings));
  g_settings.version = SETTINGS_VERSION;
  g_settings.sizeIdx = 3;   // SVGA
  g_settings.quality = 12;
  g_settings.segmentMin = 5;
  g_settings.loopRec = 1;
}

void settingsLoad() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  setDefaults();
  nvs_handle_t h;
  if (nvs_open("smoothcam", NVS_READONLY, &h) != ESP_OK) return;
  Settings stored;
  size_t len = sizeof(stored);
  if (nvs_get_blob(h, "cfg", &stored, &len) == ESP_OK && len == sizeof(stored) &&
      stored.version == SETTINGS_VERSION) {
    g_settings = stored;
  }
  nvs_close(h);
}

void settingsSave() {
  nvs_handle_t h;
  if (nvs_open("smoothcam", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "cfg", &g_settings, sizeof(g_settings));
  nvs_commit(h);
  nvs_close(h);
}
