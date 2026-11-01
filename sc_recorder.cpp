#include "sc_recorder.h"
#include "sc_avi.h"
#include "sc_camera.h"
#include "sc_config.h"
#include "sc_settings.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Each queued frame in the ring is this header followed by the JPEG data.
struct FrameHdr {
  uint32_t len;
  uint32_t ms;
  uint16_t width;
  uint16_t height;
};

#define ALIGN4(n) (((n) + 3u) & ~3u)

static uint8_t* s_ring;
static uint32_t s_ringSize;
static uint32_t s_ringMask;
static std::atomic<uint32_t> s_head{0};   // written by the capture task
static std::atomic<uint32_t> s_tail{0};   // written by the writer task

static uint8_t* s_chunk;                  // internal RAM, DMA-capable
static size_t s_chunkFill;
static AviIndexEntry* s_index;
static uint32_t s_indexCount;

static TaskHandle_t s_task;
static std::atomic<RecState> s_state{REC_NO_SD};
static std::atomic<bool> s_want{false};
static std::atomic<bool> s_accepting{false};
static std::atomic<uint32_t> s_dropped{0};
static std::atomic<uint32_t> s_ringPeak{0};
static const char* s_error = "";

static int s_fd = -1;
static int s_fileIndex;
static char s_fileName[16];
static uint64_t s_filePos;
static uint64_t s_fileLimit;
static AviInfo s_avi;
static uint32_t s_firstMs;
static uint32_t s_lastMs;
static uint32_t s_lastSync;

static uint32_t s_sessionStart;
static uint32_t s_sessionFrames;
static uint32_t s_sessionFiles;
static uint64_t s_sessionBytes;
static uint32_t s_maxWriteMs;

// ---- file naming -----------------------------------------------------------

void recFileName(int index, char* out, size_t len) {
  snprintf(out, len, "REC%05d.AVI", index);
}

void recFilePath(const char* name, char* out, size_t len) {
  snprintf(out, len, VIDEO_DIR "/%s", name);
}

// Names reach the file system from URLs, so this also has to reject paths.
bool recValidName(const char* name) {
  if (strlen(name) != 12 || strncmp(name, "REC", 3) != 0) return false;
  for (int i = 3; i < 8; i++) {
    if (!isdigit((unsigned char)name[i])) return false;
  }
  return strcasecmp(name + 8, ".AVI") == 0;
}

static int compareDescending(const void* a, const void* b) {
  return *(const int*)b - *(const int*)a;
}

int recListFiles(int* out, int max) {
  DIR* dir = opendir(VIDEO_DIR);
  if (!dir) return 0;
  int count = 0;
  while (struct dirent* entry = readdir(dir)) {
    int index;
    if (recValidName(entry->d_name) && sscanf(entry->d_name, "REC%5d", &index) == 1) {
      if (count < max) {
        out[count++] = index;
      } else {
        // Keep the newest files when there are more than the caller can hold.
        int oldest = 0;
        for (int i = 1; i < count; i++) {
          if (out[i] < out[oldest]) oldest = i;
        }
        if (index > out[oldest]) out[oldest] = index;
      }
    }
  }
  closedir(dir);
  qsort(out, count, sizeof(int), compareDescending);
  return count;
}

static int findOldestFile() {
  DIR* dir = opendir(VIDEO_DIR);
  if (!dir) return -1;
  int oldest = -1;
  while (struct dirent* entry = readdir(dir)) {
    int index;
    if (recValidName(entry->d_name) && sscanf(entry->d_name, "REC%5d", &index) == 1) {
      if (oldest < 0 || index < oldest) oldest = index;
    }
  }
  closedir(dir);
  return oldest;
}

static uint64_t sdFreeBytes() {
  return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

// ---- buffered file output --------------------------------------------------

static bool flushChunk() {
  if (s_chunkFill == 0) return true;
  const uint32_t start = millis();
  const ssize_t written = write(s_fd, s_chunk, s_chunkFill);
  const uint32_t took = millis() - start;
  if (took > s_maxWriteMs) s_maxWriteMs = took;
  if (written != (ssize_t)s_chunkFill) return false;
  s_chunkFill = 0;
  return true;
}

// Full chunks stay aligned to the file start, so with a matching cluster size
// FatFs writes whole clusters straight from this buffer.
static bool putBytes(const void* data, size_t len) {
  const uint8_t* src = (const uint8_t*)data;
  s_filePos += len;
  while (len) {
    const size_t n = std::min(len, (size_t)REC_CHUNK_BYTES - s_chunkFill);
    memcpy(s_chunk + s_chunkFill, src, n);
    s_chunkFill += n;
    src += n;
    len -= n;
    if (s_chunkFill == REC_CHUNK_BYTES && !flushChunk()) return false;
  }
  return true;
}

static void ringRead(uint32_t pos, void* out, size_t len) {
  const uint32_t offset = pos & s_ringMask;
  const size_t first = std::min(len, (size_t)(s_ringSize - offset));
  memcpy(out, s_ring + offset, first);
  if (first < len) memcpy((uint8_t*)out + first, s_ring, len - first);
}

static void ringWrite(uint32_t pos, const void* data, size_t len) {
  const uint32_t offset = pos & s_ringMask;
  const size_t first = std::min(len, (size_t)(s_ringSize - offset));
  memcpy(s_ring + offset, data, first);
  if (first < len) memcpy(s_ring, (const uint8_t*)data + first, len - first);
}

static bool putFromRing(uint32_t pos, size_t len) {
  while (len) {
    const uint32_t offset = pos & s_ringMask;
    const size_t n = std::min(len, (size_t)(s_ringSize - offset));
    if (!putBytes(s_ring + offset, n)) return false;
    pos += n;
    len -= n;
  }
  return true;
}

static bool putIndex(uint32_t count) {
  const uint32_t bytes = count * sizeof(AviIndexEntry);
  return putBytes("idx1", 4) && putBytes(&bytes, 4) && putBytes(s_index, bytes);
}

// ---- segments --------------------------------------------------------------

static bool ensureSpace(uint64_t* freeOut) {
  uint64_t freeBytes = sdFreeBytes();
  while (freeBytes < REC_MIN_FREE_BYTES) {
    const int oldest = g_settings.loopRec ? findOldestFile() : -1;
    if (oldest < 0) {
      s_error = g_settings.loopRec ? "لا توجد ملفات قديمة لحذفها والبطاقة ممتلئة" : "البطاقة ممتلئة";
      return false;
    }
    char name[16], path[48];
    recFileName(oldest, name, sizeof(name));
    recFilePath(name, path, sizeof(path));
    if (unlink(path) != 0) {
      s_error = "تعذر حذف أقدم ملف";
      return false;
    }
    freeBytes = sdFreeBytes();
  }
  *freeOut = freeBytes;
  return true;
}

static bool openSegment(const FrameHdr& frame) {
  uint64_t freeBytes;
  if (!ensureSpace(&freeBytes)) return false;

  char path[48];
  recFileName(++s_fileIndex, s_fileName, sizeof(s_fileName));
  recFilePath(s_fileName, path, sizeof(path));
  s_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (s_fd < 0) {
    s_error = "تعذر إنشاء ملف على البطاقة";
    return false;
  }

  const CamStats cam = camGetStats();
  memset(&s_avi, 0, sizeof(s_avi));
  s_avi.width = frame.width;
  s_avi.height = frame.height;
  // Only a file that is never closed keeps this rate, in the repair path.
  s_avi.fpsMilli = cam.fpsX10 ? cam.fpsX10 * 100 : 15000;

  uint8_t header[AVI_HDR_SIZE];
  aviBuildHeader(header, s_avi);
  s_chunkFill = 0;
  s_filePos = 0;
  s_indexCount = 0;
  s_firstMs = frame.ms;
  s_lastMs = frame.ms;
  s_lastSync = millis();
  s_fileLimit = std::min((uint64_t)REC_MAX_FILE_BYTES, freeBytes - 64 * 1024 * 1024);
  s_sessionFiles++;
  return putBytes(header, sizeof(header));
}

static bool finishFile() {
  s_avi.moviBytes = (uint32_t)(s_filePos - AVI_HDR_SIZE);
  s_avi.frames = s_indexCount;
  if (s_indexCount > 1 && s_lastMs != s_firstMs) {
    s_avi.fpsMilli = (uint32_t)((uint64_t)(s_indexCount - 1) * 1000000ULL / (s_lastMs - s_firstMs));
  }
  uint8_t header[AVI_HDR_SIZE];
  aviBuildHeader(header, s_avi);

  const bool ok = putIndex(s_indexCount) && flushChunk() && lseek(s_fd, 0, SEEK_SET) == 0 &&
                  write(s_fd, header, sizeof(header)) == (ssize_t)sizeof(header);
  if (close(s_fd) != 0 || !ok) {
    s_fd = -1;
    s_error = "فشل إغلاق الملف على البطاقة";
    return false;
  }
  s_fd = -1;
  return true;
}

static bool needsNewSegment(const FrameHdr& frame) {
  const uint32_t segmentMs = g_settings.segmentMin * 60000u;
  return frame.width != s_avi.width || frame.height != s_avi.height ||
         frame.ms - s_firstMs >= segmentMs || s_indexCount >= REC_MAX_FRAMES ||
         s_filePos + frame.len >= s_fileLimit;
}

static bool writeNextFrame() {
  const uint32_t tail = s_tail.load(std::memory_order_acquire);
  FrameHdr frame;
  ringRead(tail, &frame, sizeof(frame));

  if (s_fd >= 0 && needsNewSegment(frame) && !finishFile()) return false;
  if (s_fd < 0 && !openSegment(frame)) return false;

  AviIndexEntry& entry = s_index[s_indexCount];
  memcpy(entry.id, "00dc", 4);
  entry.flags = AVI_FLAG_KEY;
  entry.offset = (uint32_t)(s_filePos - AVI_MOVI_OFS);
  entry.size = frame.len;

  static const uint8_t pad = 0;
  const bool ok = putBytes("00dc", 4) && putBytes(&frame.len, 4) &&
                  putFromRing(tail + sizeof(frame), frame.len) &&
                  (!(frame.len & 1) || putBytes(&pad, 1));
  if (!ok) {
    s_error = "فشلت الكتابة على البطاقة";
    return false;
  }

  s_indexCount++;
  s_avi.maxChunk = std::max(s_avi.maxChunk, frame.len);
  s_lastMs = frame.ms;
  s_sessionFrames++;
  s_sessionBytes += frame.len;
  s_tail.store(tail + sizeof(frame) + ALIGN4(frame.len), std::memory_order_release);
  return true;
}

// ---- repair after power loss -----------------------------------------------

// Power loss leaves a file with no index and a zero frame count. Rebuild the
// index by walking the frame chunks, cutting off a partly written last frame.
static void repairLastFile() {
  int newest;
  if (recListFiles(&newest, 1) != 1) return;

  char name[16], path[48];
  recFileName(newest, name, sizeof(name));
  recFilePath(name, path, sizeof(path));
  struct stat st;
  if (stat(path, &st) != 0) return;

  s_fd = open(path, O_RDWR);
  if (s_fd < 0) return;

  uint8_t header[AVI_HDR_SIZE];
  AviInfo info;
  if (read(s_fd, header, sizeof(header)) != (ssize_t)sizeof(header) ||
      !aviParseHeader(header, &info) || info.frames != 0) {
    close(s_fd);
    s_fd = -1;
    return;
  }

  s_state = REC_REPAIRING;
  Serial.printf("repairing %s\n", name);

  uint32_t pos = AVI_HDR_SIZE;
  uint32_t count = 0;
  const uint32_t size = (uint32_t)st.st_size;
  while (pos + 8 <= size && count < REC_MAX_FRAMES) {
    uint8_t chunk[8];
    uint32_t len;
    if (lseek(s_fd, pos, SEEK_SET) < 0 || read(s_fd, chunk, 8) != 8) break;
    memcpy(&len, chunk + 4, 4);
    const uint32_t next = pos + 8 + len + (len & 1);
    if (memcmp(chunk, "00dc", 4) != 0 || len == 0 || next > size) break;
    AviIndexEntry& entry = s_index[count++];
    memcpy(entry.id, "00dc", 4);
    entry.flags = AVI_FLAG_KEY;
    entry.offset = pos - AVI_MOVI_OFS;
    entry.size = len;
    info.maxChunk = std::max(info.maxChunk, len);
    pos = next;
  }

  if (count == 0) {
    close(s_fd);
    s_fd = -1;
    unlink(path);
    return;
  }

  info.frames = count;
  info.moviBytes = pos - AVI_HDR_SIZE;
  s_filePos = pos;
  s_chunkFill = 0;
  aviBuildHeader(header, info);
  const bool ok = ftruncate(s_fd, pos) == 0 && lseek(s_fd, pos, SEEK_SET) == (off_t)pos &&
                  putIndex(count) && flushChunk() && lseek(s_fd, 0, SEEK_SET) == 0 &&
                  write(s_fd, header, sizeof(header)) == (ssize_t)sizeof(header);
  close(s_fd);
  s_fd = -1;
  Serial.printf("repair %s: %u frames\n", ok ? "done" : "failed", (unsigned)count);
}

// ---- writer task -----------------------------------------------------------

static void failRecording() {
  Serial.printf("recording failed: %s\n", s_error);
  s_accepting = false;
  s_want = false;
  if (s_fd >= 0) {
    close(s_fd);
    s_fd = -1;
  }
  s_state = REC_ERROR;
}

static void writerTask(void*) {
  repairLastFile();
  s_state = REC_IDLE;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

    if (s_want && !s_accepting) {
      s_error = "";
      s_dropped = 0;
      s_ringPeak = 0;
      s_sessionStart = millis();
      s_sessionFrames = 0;
      s_sessionFiles = 0;
      s_sessionBytes = 0;
      s_maxWriteMs = 0;
      s_state = REC_RECORDING;
      s_accepting = true;
    }

    if (!s_accepting && s_fd < 0) {
      // A frame can slip in as recording stops; it belongs to no file.
      s_tail.store(s_head.load(std::memory_order_acquire), std::memory_order_release);
      // Stopped before the first frame arrived, so no file was opened.
      if (s_state.load() == REC_STOPPING) s_state = REC_IDLE;
      continue;
    }

    bool failed = false;
    while (s_tail.load(std::memory_order_acquire) != s_head.load(std::memory_order_acquire)) {
      if (!writeNextFrame()) {
        failed = true;
        break;
      }
    }
    if (failed) {
      failRecording();
      continue;
    }

    // Commits the file size, which bounds what a power cut can lose.
    if (s_fd >= 0 && millis() - s_lastSync >= REC_SYNC_MS) {
      fsync(s_fd);
      s_lastSync = millis();
    }

    if (!s_want && s_accepting) {
      s_accepting = false;
      s_state = REC_STOPPING;
      xTaskNotifyGive(s_task);
      continue;
    }

    const bool drained = s_tail.load() == s_head.load();
    if (!s_accepting && drained) {
      if (s_fd >= 0 && !finishFile()) {
        failRecording();
        continue;
      }
      s_state = REC_IDLE;
    }
  }
}

// ---- public API ------------------------------------------------------------

void recPush(const camera_fb_t* fb, uint32_t ms) {
  if (!s_accepting.load()) return;

  const uint32_t head = s_head.load(std::memory_order_relaxed);
  const uint32_t tail = s_tail.load(std::memory_order_acquire);
  const uint32_t need = sizeof(FrameHdr) + ALIGN4(fb->len);
  const uint32_t used = head - tail;
  if (need > s_ringSize - used) {
    s_dropped++;
    return;
  }

  const FrameHdr frame = {fb->len, ms, (uint16_t)fb->width, (uint16_t)fb->height};
  ringWrite(head, &frame, sizeof(frame));
  ringWrite(head + sizeof(frame), fb->buf, fb->len);
  s_head.store(head + need, std::memory_order_release);

  if (used + need > s_ringPeak.load()) s_ringPeak = used + need;
  xTaskNotifyGive(s_task);
}

static bool mountCard() {
  for (int khz : {40000, 20000}) {
    if (SD_MMC.begin(SD_MOUNT, !SD_BUS_4BIT, false, khz, 4)) {
      if (SD_MMC.cardType() != CARD_NONE) {
        Serial.printf("SD mounted at %d kHz, %llu MB\n", khz, SD_MMC.cardSize() / (1024 * 1024));
        return true;
      }
      SD_MMC.end();
    }
  }
  return false;
}

bool recBegin() {
  s_chunk = (uint8_t*)heap_caps_malloc(REC_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_index = (AviIndexEntry*)ps_malloc(REC_MAX_FRAMES * sizeof(AviIndexEntry));
  s_ringSize = REC_RING_BYTES;
  s_ring = (uint8_t*)ps_malloc(s_ringSize);
  if (!s_ring) {
    s_ringSize /= 2;
    s_ring = (uint8_t*)ps_malloc(s_ringSize);
  }
  s_ringMask = s_ringSize - 1;
  if (!s_chunk || !s_index || !s_ring) {
    Serial.println("recorder: out of memory");
    return false;
  }

  if (!mountCard()) {
    Serial.println("recorder: no SD card");
    return false;
  }
  if (mkdir(VIDEO_DIR, 0775) != 0 && errno != EEXIST) {
    Serial.println("recorder: cannot create video folder");
    return false;
  }

  int newest;
  s_fileIndex = recListFiles(&newest, 1) == 1 ? newest : 0;
  sdFreeBytes();   // the first call scans the FAT, so do it before recording

  s_state = REC_IDLE;
  xTaskCreatePinnedToCore(writerTask, "writer", 6144, nullptr, PRIO_WRITER, &s_task, CORE_MEDIA);
  return true;
}

bool recStart() {
  if (s_state.load() == REC_NO_SD) return false;
  s_want = true;
  xTaskNotifyGive(s_task);
  return true;
}

void recStop() {
  if (s_state.load() == REC_NO_SD) return;
  s_want = false;
  xTaskNotifyGive(s_task);
}

bool recIsRecording() {
  return s_accepting.load();
}

void recStopWait(uint32_t timeoutMs) {
  recStop();
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const RecState state = s_state.load();
    if (state != REC_RECORDING && state != REC_STOPPING) return;
    delay(20);
  }
}

bool recIsActiveFile(const char* name) {
  return s_fd >= 0 && strcasecmp(name, s_fileName) == 0;
}

bool recDeleteFile(const char* name) {
  if (s_state.load() == REC_NO_SD || !recValidName(name) || recIsActiveFile(name)) return false;
  char path[48];
  recFilePath(name, path, sizeof(path));
  return unlink(path) == 0;
}

RecState recGetState() {
  return s_state.load();
}

RecStats recGetStats() {
  RecStats stats = {};
  stats.state = s_state.load();
  if (stats.state == REC_NO_SD) return stats;

  stats.dropped = s_dropped.load();
  stats.frames = s_sessionFrames;
  stats.files = s_sessionFiles;
  stats.maxWriteMs = s_maxWriteMs;
  stats.error = s_error;
  stats.ringPct = (s_head.load() - s_tail.load()) * 100 / s_ringSize;
  stats.ringPeakPct = s_ringPeak.load() * 100 / s_ringSize;
  if (stats.state == REC_RECORDING || stats.state == REC_STOPPING) {
    stats.seconds = (millis() - s_sessionStart) / 1000;
    if (stats.seconds) stats.kbPerSec = (uint32_t)(s_sessionBytes / 1024 / stats.seconds);
    strlcpy(stats.file, s_fileName, sizeof(stats.file));
  }
  stats.sdTotal = SD_MMC.totalBytes();
  stats.sdFree = sdFreeBytes();
  return stats;
}
