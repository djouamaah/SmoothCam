#include "sc_stream.h"
#include "sc_camera.h"
#include "sc_config.h"
#include "sc_recorder.h"
#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOUNDARY "smoothcamframe"
#define SEND_TIMEOUT_S 5
#define FILE_CHUNK_BYTES 8192

static std::atomic<uint32_t> s_conns{0};
static std::atomic<uint32_t> s_viewers{0};

uint32_t streamViewerCount() {
  return s_viewers.load();
}

static bool sendAll(int fd, const void* data, size_t len, int flags = 0) {
  const uint8_t* p = (const uint8_t*)data;
  while (len) {
    const ssize_t n = send(fd, p, len, flags);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

static void sendStatus(int fd, const char* status) {
  char head[128];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
  sendAll(fd, head, n);
}

static void serveStream(int fd) {
  if (s_viewers.fetch_add(1) >= MAX_VIEWERS) {
    s_viewers--;
    sendStatus(fd, "503 Service Unavailable");
    return;
  }

  static const char head[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace;boundary=" BOUNDARY "\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n";
  bool ok = sendAll(fd, head, sizeof(head) - 1);

  uint32_t seq = 0;
  while (ok) {
    // Always the newest frame: a slow client loses frames instead of falling behind.
    SharedFrame* frame = camAcquire(seq, 3000);
    if (!frame) continue;
    seq = frame->seq;

    char part[96];
    const int n = snprintf(part, sizeof(part),
                           "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                           (unsigned)frame->fb->len);
    ok = sendAll(fd, part, n, MSG_MORE) && sendAll(fd, frame->fb->buf, frame->fb->len, MSG_MORE) &&
         sendAll(fd, "\r\n", 2);
    camRelease(frame);
  }
  s_viewers--;
}

static void serveSnapshot(int fd) {
  SharedFrame* frame = camAcquire(0, 3000);
  if (!frame) {
    sendStatus(fd, "503 Service Unavailable");
    return;
  }
  char head[160];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                         (unsigned)frame->fb->len);
  if (sendAll(fd, head, n, MSG_MORE)) sendAll(fd, frame->fb->buf, frame->fb->len);
  camRelease(frame);
}

static void serveFile(int fd, const char* name, const char* request) {
  if (!recValidName(name)) {
    sendStatus(fd, "404 Not Found");
    return;
  }
  if (recIsActiveFile(name)) {
    sendStatus(fd, "409 Conflict");
    return;
  }

  char path[48];
  struct stat st;
  recFilePath(name, path, sizeof(path));
  if (stat(path, &st) != 0) {
    sendStatus(fd, "404 Not Found");
    return;
  }
  const uint32_t size = (uint32_t)st.st_size;

  uint32_t start = 0;
  uint32_t end = size ? size - 1 : 0;
  const char* range = strcasestr(request, "\r\nRange: bytes=");
  if (range) {
    unsigned long first = 0, last = 0;
    const int parsed = sscanf(range + 15, "%lu-%lu", &first, &last);
    if (parsed < 1 || first >= size) {
      sendStatus(fd, "416 Range Not Satisfiable");
      return;
    }
    start = first;
    if (parsed == 2 && last < size) end = last;
  }

  int file = open(path, O_RDONLY);
  uint8_t* buffer = (uint8_t*)heap_caps_malloc(FILE_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (file < 0 || !buffer || (start && lseek(file, start, SEEK_SET) < 0)) {
    if (file >= 0) close(file);
    free(buffer);
    sendStatus(fd, "500 Internal Server Error");
    return;
  }

  char head[300];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %s\r\nContent-Type: video/x-msvideo\r\nContent-Length: %u\r\n"
                   "Accept-Ranges: bytes\r\n"
                   "Content-Disposition: attachment; filename=\"%s\"\r\n",
                   range ? "206 Partial Content" : "200 OK", (unsigned)(end - start + 1), name);
  if (range) {
    n += snprintf(head + n, sizeof(head) - n, "Content-Range: bytes %u-%u/%u\r\n", (unsigned)start,
                  (unsigned)end, (unsigned)size);
  }
  n += snprintf(head + n, sizeof(head) - n, "Connection: close\r\n\r\n");

  bool ok = sendAll(fd, head, n);
  uint32_t remaining = end - start + 1;
  while (ok && remaining) {
    const size_t want = remaining < FILE_CHUNK_BYTES ? remaining : FILE_CHUNK_BYTES;
    const ssize_t got = read(file, buffer, want);
    if (got <= 0) break;
    ok = sendAll(fd, buffer, got);
    remaining -= got;
  }
  close(file);
  free(buffer);
}

// Reads up to the blank line ending the request headers.
static bool readRequest(int fd, char* buffer, size_t size) {
  size_t used = 0;
  while (used < size - 1) {
    const ssize_t n = recv(fd, buffer + used, size - 1 - used, 0);
    if (n <= 0) return false;
    used += n;
    buffer[used] = 0;
    if (strstr(buffer, "\r\n\r\n")) return true;
  }
  return false;
}

static void connectionTask(void* arg) {
  const int fd = (intptr_t)arg;
  const int noDelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
  timeval timeout = {SEND_TIMEOUT_S, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  char request[768];
  if (readRequest(fd, request, sizeof(request))) {
    char path[64] = "";
    sscanf(request, "GET %63[^ ?]", path);
    if (!strcmp(path, "/stream") || !strcmp(path, "/")) {
      serveStream(fd);
    } else if (!strcmp(path, "/snapshot")) {
      serveSnapshot(fd);
    } else if (!strncmp(path, "/file/", 6)) {
      serveFile(fd, path + 6, request);
    } else {
      sendStatus(fd, "404 Not Found");
    }
  }

  close(fd);
  s_conns--;
  vTaskDelete(nullptr);
}

static void listenTask(void*) {
  int server = socket(AF_INET, SOCK_STREAM, 0);
  const int reuse = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(STREAM_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(server, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(server, 2) != 0) {
    Serial.println("stream: cannot listen");
    vTaskDelete(nullptr);
  }

  for (;;) {
    const int client = accept(server, nullptr, nullptr);
    if (client < 0) continue;
    if (s_conns.load() >= MAX_CONNS) {
      sendStatus(client, "503 Service Unavailable");
      close(client);
      continue;
    }
    s_conns++;
    if (xTaskCreatePinnedToCore(connectionTask, "conn", 6144, (void*)(intptr_t)client, PRIO_STREAM,
                                nullptr, CORE_NET) != pdPASS) {
      s_conns--;
      close(client);
    }
  }
}

void streamBegin() {
  xTaskCreatePinnedToCore(listenTask, "listen", 3072, nullptr, PRIO_STREAM, nullptr, CORE_NET);
}
