#include "sc_net.h"
#include "sc_config.h"
#include "sc_settings.h"
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <time.h>

static bool s_apActive;
static bool s_announced;
static uint32_t s_lastTry;

// Power save parks the radio between beacons, the usual cause of stream stutter.
static void disableRadioSleep() {
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static void startAccessPoint() {
  char ssid[32];
  snprintf(ssid, sizeof(ssid), NET_AP_PREFIX "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  WiFi.mode(g_settings.ssid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(ssid, NET_AP_PASSWORD);
  disableRadioSleep();
  s_apActive = true;
  Serial.printf("access point \"%s\" at %s\n", ssid, WiFi.softAPIP().toString().c_str());
}

void netBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(NET_HOSTNAME);

  if (!g_settings.ssid[0]) {
    startAccessPoint();
    return;
  }

  WiFi.mode(WIFI_STA);
  disableRadioSleep();
  WiFi.begin(g_settings.ssid, g_settings.pass);
  Serial.printf("joining \"%s\"", g_settings.ssid);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < NET_CONNECT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) startAccessPoint();
  s_lastTry = millis();
}

void netLoop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (s_apActive) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      disableRadioSleep();
      s_apActive = false;
    }
    if (!s_announced) {
      s_announced = true;
      MDNS.begin(NET_HOSTNAME);
      MDNS.addService("http", "tcp", WEB_PORT);
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      Serial.printf("connected: http://%s  http://" NET_HOSTNAME ".local\n",
                    WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (!g_settings.ssid[0] || millis() - s_lastTry < NET_RETRY_MS) return;
  s_lastTry = millis();
  // Scanning for the router moves the radio off channel, which would cut off
  // anyone using the hosted network right now.
  if (s_apActive && WiFi.softAPgetStationNum() > 0) return;
  WiFi.disconnect();
  WiFi.begin(g_settings.ssid, g_settings.pass);
}

bool netIsAccessPoint() {
  return s_apActive;
}

String netIp() {
  return (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
}

int netRssi() {
  return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
}
