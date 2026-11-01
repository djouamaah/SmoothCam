#include "sc_camera.h"
#include "sc_config.h"
#include "sc_recorder.h"
#include "sc_settings.h"
#include <Arduino.h>
#include <atomic>
#include <driver/ledc.h>
#include <esp_attr.h>

const CamSize CAM_SIZES[] = {
    {"QVGA", FRAMESIZE_QVGA, 320, 240},
    {"CIF", FRAMESIZE_CIF, 400, 296},
    {"VGA", FRAMESIZE_VGA, 640, 480},
    {"SVGA", FRAMESIZE_SVGA, 800, 600},
    {"XGA", FRAMESIZE_XGA, 1024, 768},
    {"HD", FRAMESIZE_HD, 1280, 720},
};
const uint8_t CAM_SIZE_COUNT = sizeof(CAM_SIZES) / sizeof(CAM_SIZES[0]);

// Keeps recording across a self-healing reboot; survives restart, not power loss.
#define RESUME_MAGIC 0x52454331
RTC_NOINIT_ATTR uint32_t g_resumeRecording;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SharedFrame s_slots[CAM_FB_COUNT + 1];
static SharedFrame* s_latest;
static uint32_t s_seq;
static TaskHandle_t s_waiters[MAX_CONNS];

static std::atomic<uint32_t> s_capMs{0};
static std::atomic<uint32_t> s_skipFrames{0};
static std::atomic<uint32_t> s_fpsX10{0};
static std::atomic<uint32_t> s_frames{0};
static std::atomic<uint32_t> s_lastBytes{0};
static std::atomic<uint32_t> s_errors{0};
static uint8_t s_appliedSize = 0xFF;
static uint8_t s_flash;

// Call with s_mux held.
static void waiterAdd(TaskHandle_t task) {
  for (auto& w : s_waiters) {
    if (w == task) return;
  }
  for (auto& w : s_waiters) {
    if (!w) {
      w = task;
      return;
    }
  }
}

// Call with s_mux held.
static void waiterRemove(TaskHandle_t task) {
  for (auto& w : s_waiters) {
    if (w == task) w = nullptr;
  }
}

SharedFrame* camAcquire(uint32_t afterSeq, uint32_t timeoutMs) {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  for (;;) {
    portENTER_CRITICAL(&s_mux);
    if (s_latest && s_latest->seq != afterSeq) {
      SharedFrame* frame = s_latest;
      frame->refs++;
      waiterRemove(self);
      portEXIT_CRITICAL(&s_mux);
      return frame;
    }
    waiterAdd(self);
    portEXIT_CRITICAL(&s_mux);

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMs)) == 0) {
      portENTER_CRITICAL(&s_mux);
      waiterRemove(self);
      portEXIT_CRITICAL(&s_mux);
      return nullptr;
    }
  }
}

void camRelease(SharedFrame* frame) {
  camera_fb_t* done = nullptr;
  portENTER_CRITICAL(&s_mux);
  if (--frame->refs == 0) {
    done = frame->fb;
    frame->fb = nullptr;
  }
  portEXIT_CRITICAL(&s_mux);
  if (done) esp_camera_fb_return(done);
}

// Makes fb the frame viewers see next and wakes them. After this the capture
// task may keep reading fb until its next publish, since only it replaces s_latest.
static bool publish(camera_fb_t* fb) {
  TaskHandle_t wake[MAX_CONNS] = {};
  SharedFrame* previous;

  portENTER_CRITICAL(&s_mux);
  SharedFrame* slot = nullptr;
  for (auto& s : s_slots) {
    if (s.refs == 0) {
      slot = &s;
      break;
    }
  }
  if (!slot) {
    portEXIT_CRITICAL(&s_mux);
    return false;
  }
  slot->fb = fb;
  slot->refs = 1;
  slot->seq = ++s_seq;
  previous = s_latest;
  s_latest = slot;
  memcpy(wake, s_waiters, sizeof(wake));
  memset(s_waiters, 0, sizeof(s_waiters));
  portEXIT_CRITICAL(&s_mux);

  if (previous) camRelease(previous);
  for (auto task : wake) {
    if (task) xTaskNotifyGive(task);
  }
  return true;
}

static void rebootToRecover() {
  Serial.println("camera stopped responding, rebooting");
  if (recIsRecording()) g_resumeRecording = RESUME_MAGIC;
  recStopWait(5000);
  esp_restart();
}

static void captureTask(void*) {
  uint32_t nextDue = 0;
  uint32_t windowStart = millis();
  uint32_t windowFrames = 0;
  int failures = 0;

  for (;;) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      s_errors++;
      if (++failures >= CAM_MAX_FAILS) rebootToRecover();
      continue;
    }
    failures = 0;

    // The sensor emits a few broken frames after a resolution change.
    if (s_skipFrames.load() > 0) {
      s_skipFrames--;
      esp_camera_fb_return(fb);
      continue;
    }

    const uint32_t now = millis();
    const uint32_t capMs = s_capMs.load();
    if (capMs) {
      if ((int32_t)(now - nextDue) < 0) {
        esp_camera_fb_return(fb);
        continue;
      }
      nextDue += capMs;
      if ((int32_t)(now - nextDue) > (int32_t)capMs) nextDue = now + capMs;
    }

    if (!publish(fb)) {
      esp_camera_fb_return(fb);
      continue;
    }
    recPush(fb, now);

    s_frames++;
    s_lastBytes = fb->len;
    windowFrames++;
    if (now - windowStart >= 1000) {
      s_fpsX10 = windowFrames * 10000 / (now - windowStart);
      windowStart = now;
      windowFrames = 0;
    }
  }
}

static void flashInit() {
#if !SD_BUS_4BIT
  // The camera clock uses timer 0 / channel 0.
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_3;
  timer.freq_hz = 5000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&timer);

  ledc_channel_config_t channel = {};
  channel.gpio_num = PIN_FLASH_LED;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_7;
  channel.timer_sel = LEDC_TIMER_3;
  channel.duty = 0;
  ledc_channel_config(&channel);
#endif
}

void camSetFlash(uint8_t level) {
#if !SD_BUS_4BIT
  s_flash = level;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, level);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
#endif
}

uint8_t camGetFlash() {
  return s_flash;
}

void camApplySettings() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  const Settings& cfg = g_settings;

  if (cfg.sizeIdx != s_appliedSize && cfg.sizeIdx < CAM_SIZE_COUNT) {
    s_skipFrames = 3;
    s->set_framesize(s, CAM_SIZES[cfg.sizeIdx].size);
    s_appliedSize = cfg.sizeIdx;
  }
  s->set_quality(s, cfg.quality);
  s->set_brightness(s, cfg.brightness);
  s->set_contrast(s, cfg.contrast);
  s->set_saturation(s, cfg.saturation);
  s->set_vflip(s, cfg.vflip);
  s->set_hmirror(s, cfg.hmirror);
  s_capMs = cfg.fpsCap ? 1000 / cfg.fpsCap : 0;
}

bool camBegin() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = PIN_CAM_D0;
  c.pin_d1 = PIN_CAM_D1;
  c.pin_d2 = PIN_CAM_D2;
  c.pin_d3 = PIN_CAM_D3;
  c.pin_d4 = PIN_CAM_D4;
  c.pin_d5 = PIN_CAM_D5;
  c.pin_d6 = PIN_CAM_D6;
  c.pin_d7 = PIN_CAM_D7;
  c.pin_xclk = PIN_CAM_XCLK;
  c.pin_pclk = PIN_CAM_PCLK;
  c.pin_vsync = PIN_CAM_VSYNC;
  c.pin_href = PIN_CAM_HREF;
  c.pin_sccb_sda = PIN_CAM_SDA;
  c.pin_sccb_scl = PIN_CAM_SCL;
  c.pin_pwdn = PIN_CAM_PWDN;
  c.pin_reset = PIN_CAM_RESET;
  c.xclk_freq_hz = CAM_XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = CAM_MAX_FRAMESIZE;
  c.jpeg_quality = g_settings.quality;
  c.fb_count = CAM_FB_COUNT;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_aec2(s, 0);   // the DSP exposure mode can stretch frame time in low light
  s->set_gainceiling(s, GAINCEILING_16X);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_dcw(s, 1);

  flashInit();
  camApplySettings();
  return true;
}

void camStartCapture() {
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, PRIO_CAPTURE, nullptr, CORE_MEDIA);
}

CamStats camGetStats() {
  CamStats stats;
  stats.fpsX10 = s_fpsX10.load();
  stats.frames = s_frames.load();
  stats.lastBytes = s_lastBytes.load();
  stats.errors = s_errors.load();
  return stats;
}

bool camTakeResumeFlag() {
  const bool resume = g_resumeRecording == RESUME_MAGIC;
  g_resumeRecording = 0;
  return resume;
}
