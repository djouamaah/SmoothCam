// Owns the sensor and hands out each new frame without copying it.
#pragma once
#include <esp_camera.h>

struct SharedFrame {
  camera_fb_t* fb;
  uint32_t refs;
  uint32_t seq;
};

struct CamSize {
  const char* name;
  framesize_t size;
  uint16_t width;
  uint16_t height;
};

struct CamStats {
  uint32_t fpsX10;
  uint32_t frames;
  uint32_t lastBytes;
  uint32_t errors;
};

extern const CamSize CAM_SIZES[];
extern const uint8_t CAM_SIZE_COUNT;

bool camBegin();
void camStartCapture();

// Blocks until a frame newer than afterSeq exists. Pass the result to camRelease().
SharedFrame* camAcquire(uint32_t afterSeq, uint32_t timeoutMs);
void camRelease(SharedFrame* frame);

void camApplySettings();
void camSetFlash(uint8_t level);
uint8_t camGetFlash();
CamStats camGetStats();

// True once after a reboot that interrupted a recording.
bool camTakeResumeFlag();
