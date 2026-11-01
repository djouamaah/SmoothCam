#include "sc_web.h"
#include "sc_camera.h"
#include "sc_config.h"
#include "sc_net.h"
#include "sc_recorder.h"
#include "sc_settings.h"
#include "sc_stream.h"
#include "sc_web_ui.h"
#include <Arduino.h>
#include <esp_http_server.h>
#include <sys/stat.h>
#include <sys/time.h>

#define MAX_LISTED_FILES 300

static volatile uint32_t s_rebootAt;

uint32_t webRebootAt() {
  return s_rebootAt;
}

static esp_err_t sendJson(httpd_req_t* req, const char* body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, body);
}

static esp_err_t sendOk(httpd_req_t* req) {
  return sendJson(req, "{\"ok\":true}");
}

static bool queryInt(const char* query, const char* key, int* out) {
  char value[12];
  if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return false;
  *out = atoi(value);
  return true;
}

static void urlDecode(char* s) {
  char* out = s;
  for (char* in = s; *in; in++) {
    if (*in == '+') {
      *out++ = ' ';
    } else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
      const char hex[3] = {in[1], in[2], 0};
      *out++ = (char)strtol(hex, nullptr, 16);
      in += 2;
    } else {
      *out++ = *in;
    }
  }
  *out = 0;
}

// Writes s into a JSON string, escaping only what can break out of it.
static void jsonEscape(const char* s, char* out, size_t size) {
  size_t used = 0;
  for (; *s && used + 3 < size; s++) {
    if (*s == '"' || *s == '\\') out[used++] = '\\';
    out[used++] = *s;
  }
  out[used] = 0;
}

static esp_err_t handleIndex(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t handleStatus(httpd_req_t* req) {
  const CamStats cam = camGetStats();
  const RecStats rec = recGetStats();
  const Settings& cfg = g_settings;
  char ssid[70];
  jsonEscape(cfg.ssid, ssid, sizeof(ssid));

  char body[1100];
  snprintf(body, sizeof(body),
           "{\"fps\":%u.%u,\"frameKB\":%u,\"camErrors\":%u,\"viewers\":%u,\"uptime\":%lu,"
           "\"heapKB\":%u,\"psramKB\":%u,\"flash\":%u,"
           "\"rec\":{\"state\":%d,\"seconds\":%u,\"frames\":%u,\"dropped\":%u,\"files\":%u,"
           "\"kbps\":%u,\"ring\":%u,\"ringPeak\":%u,\"maxWriteMs\":%u,\"file\":\"%s\",\"error\":\"%s\"},"
           "\"sd\":{\"totalMB\":%llu,\"freeMB\":%llu},"
           "\"net\":{\"ap\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
           "\"cfg\":{\"size\":\"%s\",\"quality\":%u,\"fpsCap\":%u,\"brightness\":%d,\"contrast\":%d,"
           "\"saturation\":%d,\"vflip\":%u,\"hmirror\":%u,\"segmentMin\":%u,\"autoRec\":%u,\"loopRec\":%u}}",
           (unsigned)(cam.fpsX10 / 10), (unsigned)(cam.fpsX10 % 10), (unsigned)(cam.lastBytes / 1024),
           (unsigned)cam.errors, (unsigned)streamViewerCount(), millis() / 1000,
           (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)camGetFlash(), (int)rec.state, (unsigned)rec.seconds, (unsigned)rec.frames,
           (unsigned)rec.dropped, (unsigned)rec.files, (unsigned)rec.kbPerSec, (unsigned)rec.ringPct,
           (unsigned)rec.ringPeakPct, (unsigned)rec.maxWriteMs, rec.file, rec.error ? rec.error : "",
           rec.sdTotal / (1024 * 1024), rec.sdFree / (1024 * 1024), netIsAccessPoint() ? "true" : "false",
           ssid, netIp().c_str(), netRssi(), CAM_SIZES[cfg.sizeIdx].name, cfg.quality, cfg.fpsCap,
           cfg.brightness, cfg.contrast, cfg.saturation, cfg.vflip, cfg.hmirror, cfg.segmentMin,
           cfg.autoRec, cfg.loopRec);
  return sendJson(req, body);
}

static esp_err_t handleSet(httpd_req_t* req) {
  char query[200];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
  }
  Settings& cfg = g_settings;
  char size[8];
  int v;
  if (httpd_query_key_value(query, "size", size, sizeof(size)) == ESP_OK) {
    for (uint8_t i = 0; i < CAM_SIZE_COUNT; i++) {
      if (!strcmp(size, CAM_SIZES[i].name)) cfg.sizeIdx = i;
    }
  }
  if (queryInt(query, "quality", &v)) cfg.quality = constrain(v, 6, 40);
  if (queryInt(query, "fpsCap", &v)) cfg.fpsCap = constrain(v, 0, 60);
  if (queryInt(query, "brightness", &v)) cfg.brightness = constrain(v, -2, 2);
  if (queryInt(query, "contrast", &v)) cfg.contrast = constrain(v, -2, 2);
  if (queryInt(query, "saturation", &v)) cfg.saturation = constrain(v, -2, 2);
  if (queryInt(query, "vflip", &v)) cfg.vflip = v ? 1 : 0;
  if (queryInt(query, "hmirror", &v)) cfg.hmirror = v ? 1 : 0;
  if (queryInt(query, "segmentMin", &v)) cfg.segmentMin = constrain(v, 1, 10);
  if (queryInt(query, "autoRec", &v)) cfg.autoRec = v ? 1 : 0;
  if (queryInt(query, "loopRec", &v)) cfg.loopRec = v ? 1 : 0;

  // The flash is a momentary control, so it is not saved.
  if (queryInt(query, "flash", &v)) {
    camSetFlash(constrain(v, 0, 255));
    return sendOk(req);
  }
  camApplySettings();
  settingsSave();
  return sendOk(req);
}

static esp_err_t handleRecord(httpd_req_t* req) {
  char query[16];
  int on = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) queryInt(query, "on", &on);
  if (on) {
    if (!recStart()) {
      httpd_resp_set_status(req, "409 Conflict");
      return httpd_resp_sendstr(req, "no SD card");
    }
  } else {
    recStop();
  }
  return sendOk(req);
}

static esp_err_t handleFiles(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr_chunk(req, "[");

  int* indices = (int*)ps_malloc(MAX_LISTED_FILES * sizeof(int));
  const int count = indices ? recListFiles(indices, MAX_LISTED_FILES) : 0;
  bool first = true;
  for (int i = 0; i < count; i++) {
    char name[16], path[48], row[96];
    struct stat st;
    recFileName(indices[i], name, sizeof(name));
    recFilePath(name, path, sizeof(path));
    if (stat(path, &st) != 0) continue;
    snprintf(row, sizeof(row), "%s{\"name\":\"%s\",\"size\":%lu,\"time\":%lld,\"active\":%s}",
             first ? "" : ",", name, (unsigned long)st.st_size, (long long)st.st_mtime,
             recIsActiveFile(name) ? "true" : "false");
    first = false;
    if (httpd_resp_sendstr_chunk(req, row) != ESP_OK) break;
  }
  free(indices);

  httpd_resp_sendstr_chunk(req, "]");
  return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t handleDelete(httpd_req_t* req) {
  char query[48], name[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !recDeleteFile(name)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cannot delete");
  }
  return sendOk(req);
}

// Lets a browser give the device the time when there is no internet for NTP.
static esp_err_t handleTime(httpd_req_t* req) {
  char query[32], value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "t", value, sizeof(value)) == ESP_OK) {
    timeval now = {};
    time(&now.tv_sec);
    // Anything before 2020 means the clock was never set.
    if (now.tv_sec < 1577836800) {
      now.tv_sec = atoll(value);
      settimeofday(&now, nullptr);
    }
  }
  return sendOk(req);
}

static esp_err_t handleWifi(httpd_req_t* req) {
  char body[300];
  if (req->content_len == 0 || req->content_len >= sizeof(body)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
  }
  int received = 0;
  while (received < (int)req->content_len) {
    const int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) return ESP_FAIL;
    received += n;
  }
  body[received] = 0;

  char ssid[100] = "", pass[200] = "";
  httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
  httpd_query_key_value(body, "pass", pass, sizeof(pass));
  urlDecode(ssid);
  urlDecode(pass);
  if (strlen(ssid) >= sizeof(g_settings.ssid) || strlen(pass) >= sizeof(g_settings.pass)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
  }

  strlcpy(g_settings.ssid, ssid, sizeof(g_settings.ssid));
  strlcpy(g_settings.pass, pass, sizeof(g_settings.pass));
  settingsSave();
  s_rebootAt = millis() + 1000;
  return sendOk(req);
}

static esp_err_t handleReboot(httpd_req_t* req) {
  s_rebootAt = millis() + 500;
  return sendOk(req);
}

void webBegin() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = WEB_PORT;
  config.max_uri_handlers = 12;
  config.stack_size = 8192;
  config.core_id = CORE_NET;
  config.task_priority = PRIO_WEB;
  config.lru_purge_enable = true;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    Serial.println("web server failed to start");
    return;
  }

  const httpd_uri_t routes[] = {
      {"/", HTTP_GET, handleIndex, nullptr},
      {"/api/status", HTTP_GET, handleStatus, nullptr},
      {"/api/files", HTTP_GET, handleFiles, nullptr},
      {"/api/set", HTTP_POST, handleSet, nullptr},
      {"/api/record", HTTP_POST, handleRecord, nullptr},
      {"/api/delete", HTTP_POST, handleDelete, nullptr},
      {"/api/time", HTTP_POST, handleTime, nullptr},
      {"/api/wifi", HTTP_POST, handleWifi, nullptr},
      {"/api/reboot", HTTP_POST, handleReboot, nullptr},
  };
  for (const auto& route : routes) httpd_register_uri_handler(server, &route);
}
