// Options changed from the web UI and kept across reboots in NVS.
#pragma once
#include <stdint.h>

struct Settings {
  uint32_t version;
  char ssid[33];
  char pass[65];
  uint8_t sizeIdx;      // index into CAM_SIZES
  uint8_t quality;      // JPEG quality, lower is better and larger
  uint8_t fpsCap;       // 0 = as fast as the sensor runs
  int8_t brightness;
  int8_t contrast;
  int8_t saturation;
  uint8_t vflip;
  uint8_t hmirror;
  uint8_t segmentMin;   // minutes per video file
  uint8_t autoRec;      // start recording at boot
  uint8_t loopRec;      // delete oldest files when the card is full
};

extern Settings g_settings;

void settingsLoad();
void settingsSave();
