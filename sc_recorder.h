// Records to SD as segmented MJPEG AVI files. Frames pass through a PSRAM ring,
// so a slow card write never holds up the camera or the live stream.
#pragma once
#include <esp_camera.h>
#include <stdint.h>

enum RecState : uint8_t {
  REC_NO_SD,
  REC_REPAIRING,
  REC_IDLE,
  REC_RECORDING,
  REC_STOPPING,
  REC_ERROR,
};

struct RecStats {
  RecState state;
  uint32_t seconds;
  uint32_t frames;
  uint32_t dropped;
  uint32_t files;
  uint32_t kbPerSec;
  uint32_t ringPct;
  uint32_t ringPeakPct;
  uint32_t maxWriteMs;
  char file[16];
  const char* error;
  uint64_t sdTotal;
  uint64_t sdFree;
};

bool recBegin();
bool recStart();
void recStop();
void recStopWait(uint32_t timeoutMs);
bool recIsRecording();

// Capture task only.
void recPush(const camera_fb_t* fb, uint32_t ms);

RecStats recGetStats();
RecState recGetState();

// Fills out with file numbers, newest first, and returns how many were found.
int recListFiles(int* out, int max);
void recFileName(int index, char* out, size_t len);
void recFilePath(const char* name, char* out, size_t len);
bool recValidName(const char* name);
bool recIsActiveFile(const char* name);
bool recDeleteFile(const char* name);
