// Wi-Fi: joins the saved network, or hosts its own until that network is back.
#pragma once
#include <WiFi.h>

void netBegin();
void netLoop();

bool netIsAccessPoint();
String netIp();
int netRssi();
