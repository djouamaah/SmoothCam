// Web UI and JSON control API on WEB_PORT.
#pragma once
#include <stdint.h>

void webBegin();

// Non-zero once a reboot was requested from the UI; the value is its due time.
uint32_t webRebootAt();
