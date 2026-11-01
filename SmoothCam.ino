// SmoothCam: smooth live video and SD recording for the AI-Thinker ESP32-CAM.
// Board: "AI Thinker ESP32-CAM". See README.md for setup.

#include "sc_camera.h"
#include "sc_config.h"
#include "sc_net.h"
#include "sc_recorder.h"
#include "sc_settings.h"
#include "sc_stream.h"
#include "sc_web.h"

static void haltWithBlink(const char* reason) {
  Serial.println(reason);
  pinMode(PIN_STATUS_LED, OUTPUT);
  for (bool lit = true;; lit = !lit) {
    digitalWrite(PIN_STATUS_LED, lit ? LOW : HIGH);
    delay(150);
  }
}

// On while recording, slow blink after a recording error.
static void updateStatusLed() {
  const RecState state = recGetState();
  const bool blink = (millis() / 500) & 1;
  const bool lit = state == REC_RECORDING || state == REC_STOPPING || (state == REC_ERROR && blink);
  digitalWrite(PIN_STATUS_LED, lit ? LOW : HIGH);
}

// A health line for diagnosing the board with no network, over USB alone.
static void logStatus() {
  static uint32_t last;
  if (millis() - last < 5000) return;
  last = millis();
  const CamStats cam = camGetStats();
  Serial.printf("fps %u.%u  frame %u KB  frames %u  errors %u  viewers %u  heap %u KB\n",
                (unsigned)(cam.fpsX10 / 10), (unsigned)(cam.fpsX10 % 10),
                (unsigned)(cam.lastBytes / 1024), (unsigned)cam.frames, (unsigned)cam.errors,
                (unsigned)streamViewerCount(), (unsigned)(ESP.getFreeHeap() / 1024));
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nSmoothCam starting");
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, HIGH);

  settingsLoad();
  if (!psramFound()) haltWithBlink("PSRAM not found: select the AI Thinker ESP32-CAM board");
  if (!camBegin()) haltWithBlink("camera not found: check the ribbon cable and power");

  // Before Wi-Fi, which fragments the internal RAM the recorder's DMA buffer needs.
  const bool recorderReady = recBegin();

  netBegin();
  webBegin();
  streamBegin();
  camStartCapture();

  const bool resume = camTakeResumeFlag();
  if (recorderReady && (g_settings.autoRec || resume)) recStart();

  Serial.printf("free: %u KB heap, %u KB PSRAM\n", (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024));
}

void loop() {
  netLoop();
  updateStatusLed();
  logStatus();

  const uint32_t rebootAt = webRebootAt();
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    recStopWait(5000);
    ESP.restart();
  }
  delay(50);
}
