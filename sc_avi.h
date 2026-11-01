// Minimal MJPEG AVI container. The header has a fixed size, so the recorder can
// write it with placeholder counts and rewrite it in place when a file closes.
#pragma once
#include <stdint.h>
#include <string.h>

#define AVI_HDR_SIZE  512
#define AVI_MOVI_OFS  508   // file offset of 'movi'; idx1 offsets count from here
#define AVI_FLAG_KEY  0x10

struct AviIndexEntry {
  char id[4];
  uint32_t flags;
  uint32_t offset;
  uint32_t size;
};

struct AviInfo {
  uint32_t width;
  uint32_t height;
  uint32_t fpsMilli;    // frames per 1000 seconds
  uint32_t frames;      // 0 marks a file that was never closed
  uint32_t moviBytes;   // bytes after the 'movi' fourcc
  uint32_t maxChunk;
};

static inline void aviPut32(uint8_t*& p, uint32_t v) {
  memcpy(p, &v, 4);   // the ESP32 is little-endian, like AVI
  p += 4;
}

static inline void aviPutCC(uint8_t*& p, const char* cc) {
  memcpy(p, cc, 4);
  p += 4;
}

static inline void aviPut16(uint8_t*& p, uint16_t v) {
  memcpy(p, &v, 2);
  p += 2;
}

static inline void aviBuildHeader(uint8_t* out, const AviInfo& a) {
  const uint32_t idxBytes = a.frames * sizeof(AviIndexEntry);
  const uint32_t fps = a.fpsMilli ? a.fpsMilli : 1000;
  uint8_t* p = out;
  memset(out, 0, AVI_HDR_SIZE);

  aviPutCC(p, "RIFF");
  aviPut32(p, AVI_HDR_SIZE - 8 + a.moviBytes + (a.frames ? 8 + idxBytes : 0));
  aviPutCC(p, "AVI ");

  aviPutCC(p, "LIST");
  aviPut32(p, 192);
  aviPutCC(p, "hdrl");

  aviPutCC(p, "avih");
  aviPut32(p, 56);
  aviPut32(p, (uint32_t)(1000000000ULL / fps));   // dwMicroSecPerFrame
  aviPut32(p, 0);                                 // dwMaxBytesPerSec
  aviPut32(p, 0);                                 // dwPaddingGranularity
  aviPut32(p, 0x10);                              // AVIF_HASINDEX
  aviPut32(p, a.frames);
  aviPut32(p, 0);                                 // dwInitialFrames
  aviPut32(p, 1);                                 // dwStreams
  aviPut32(p, a.maxChunk);
  aviPut32(p, a.width);
  aviPut32(p, a.height);
  p += 16;                                        // reserved

  aviPutCC(p, "LIST");
  aviPut32(p, 116);
  aviPutCC(p, "strl");

  aviPutCC(p, "strh");
  aviPut32(p, 56);
  aviPutCC(p, "vids");
  aviPutCC(p, "MJPG");
  aviPut32(p, 0);                                 // dwFlags
  aviPut32(p, 0);                                 // wPriority, wLanguage
  aviPut32(p, 0);                                 // dwInitialFrames
  aviPut32(p, 1000);                              // dwScale
  aviPut32(p, fps);                               // dwRate
  aviPut32(p, 0);                                 // dwStart
  aviPut32(p, a.frames);                          // dwLength
  aviPut32(p, a.maxChunk);
  aviPut32(p, 0xFFFFFFFF);                        // dwQuality
  aviPut32(p, 0);                                 // dwSampleSize
  aviPut16(p, 0);                                 // rcFrame
  aviPut16(p, 0);
  aviPut16(p, (uint16_t)a.width);
  aviPut16(p, (uint16_t)a.height);

  aviPutCC(p, "strf");
  aviPut32(p, 40);
  aviPut32(p, 40);                                // biSize
  aviPut32(p, a.width);
  aviPut32(p, a.height);
  aviPut16(p, 1);                                 // biPlanes
  aviPut16(p, 24);                                // biBitCount
  aviPutCC(p, "MJPG");
  aviPut32(p, a.width * a.height * 3);            // biSizeImage
  p += 16;                                        // resolution and palette fields

  aviPutCC(p, "JUNK");
  aviPut32(p, AVI_HDR_SIZE - 12 - (uint32_t)(p + 4 - out));
  p = out + AVI_MOVI_OFS - 8;

  aviPutCC(p, "LIST");
  aviPut32(p, 4 + a.moviBytes);
  aviPutCC(p, "movi");
}

// Reads back what the recorder needs from a header on disk.
static inline bool aviParseHeader(const uint8_t* in, AviInfo* a) {
  if (memcmp(in, "RIFF", 4) || memcmp(in + 8, "AVI ", 4) ||
      memcmp(in + AVI_MOVI_OFS, "movi", 4)) {
    return false;
  }
  uint32_t scale, rate;
  memcpy(&a->frames, in + 48, 4);
  memcpy(&a->width, in + 64, 4);
  memcpy(&a->height, in + 68, 4);
  memcpy(&scale, in + 128, 4);
  memcpy(&rate, in + 132, 4);
  a->fpsMilli = scale ? rate * 1000 / scale : 1000;
  a->moviBytes = 0;
  a->maxChunk = 0;
  return true;
}
