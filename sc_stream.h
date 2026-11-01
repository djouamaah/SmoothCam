// Long-lived responses on STREAM_PORT: live MJPEG, snapshots and file downloads.
// Each connection has its own task, so a stalled client delays nobody else.
#pragma once
#include <stdint.h>

void streamBegin();
uint32_t streamViewerCount();
