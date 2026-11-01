// SmoothCam build-time configuration for the AI-Thinker ESP32-CAM.
// Runtime options (resolution, Wi-Fi, ...) live in the web UI, not here.
#pragma once

// ---- AI-Thinker pin map ----------------------------------------------------
#define PIN_CAM_PWDN    32
#define PIN_CAM_RESET   -1
#define PIN_CAM_XCLK    0
#define PIN_CAM_SDA     26
#define PIN_CAM_SCL     27
#define PIN_CAM_D0      5
#define PIN_CAM_D1      18
#define PIN_CAM_D2      19
#define PIN_CAM_D3      21
#define PIN_CAM_D4      36
#define PIN_CAM_D5      39
#define PIN_CAM_D6      34
#define PIN_CAM_D7      35
#define PIN_CAM_VSYNC   25
#define PIN_CAM_HREF    23
#define PIN_CAM_PCLK    22
#define PIN_FLASH_LED   4
#define PIN_STATUS_LED  33   // active low

// ---- Camera ----------------------------------------------------------------
#define CAM_XCLK_HZ        20000000
// Buffers are sized for this at init; the UI can only pick this size or smaller.
#define CAM_MAX_FRAMESIZE  FRAMESIZE_HD
// Consecutive failed captures (4 s each) before a self-healing reboot.
#define CAM_MAX_FAILS      3

// ---- Network ---------------------------------------------------------------
#define NET_HOSTNAME       "smoothcam"
#define NET_AP_PREFIX      "SmoothCam-"
#define NET_AP_PASSWORD    "smoothcam"     // at least 8 characters
#define NET_CONNECT_MS     15000
#define NET_RETRY_MS       60000
#define WEB_PORT           80
#define STREAM_PORT        81
#define MAX_VIEWERS        2               // live video clients
#define MAX_CONNS          3               // viewers + downloads + snapshots

// A slow viewer pins at most one buffer, so this count guarantees the camera
// always has one to fill: latest + one per connection + filling + queued.
#define CAM_FB_COUNT       (MAX_CONNS + 3)

// ---- SD card / recorder ----------------------------------------------------
// 0: 1-bit bus, flash LED stays usable. 1: 4-bit bus, faster, but the flash LED
// shares GPIO4 and flickers during writes and cannot be controlled.
#define SD_BUS_4BIT        0
#define SD_MOUNT           "/sdcard"
#define VIDEO_DIR          "/sdcard/video"
#define REC_RING_BYTES     (2u * 1024 * 1024)   // PSRAM; must be a power of two
#define REC_CHUNK_BYTES    (32 * 1024)          // internal RAM; match SD cluster size
#define REC_MAX_FRAMES     18000                // per file: 10 min at 30 fps
#define REC_SYNC_MS        5000                 // data at risk on power loss
#define REC_MIN_FREE_BYTES (256ULL * 1024 * 1024)
#define REC_MAX_FILE_BYTES (1900ULL * 1024 * 1024)

// ---- Tasks -----------------------------------------------------------------
// The radio, TCP/IP stack and the camera driver's own task run on core 0 (fixed
// in the prebuilt Arduino libraries), so frame handling and SD writes use core 1.
#define CORE_NET       0
#define CORE_MEDIA     1
#define PRIO_CAPTURE   7
#define PRIO_WRITER    5
#define PRIO_STREAM    4
#define PRIO_WEB       3
