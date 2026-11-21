#include "FloodTest.h"
#include <stdio.h>
#include <string.h>

static bool word(const char*& text, char* out, size_t size) {
  while (*text == ' ') ++text;
  size_t length = 0;
  while (*text && *text != ' ') {
    if (length + 1 >= size) return false;
    out[length++] = *text++;
  }
  out[length] = 0;
  return true;
}

FloodTest::Command FloodTest::parse(const char* text, bool local, Request& request,
                                    char* reply, size_t size) {
  if (strncmp(text, "flood", 5) || (text[5] && text[5] != ' ')) return NotHandled;
  if (!local) {
    snprintf(reply, size, "Err - flood is local-terminal only");
    return Invalid;
  }
  text += 5;
  const char* usage = "Err - use flood [--continuous] <seconds 1-7200> [region], status, or stop";
  char first[31], second[31], extra[2];
  if (!word(text, first, sizeof(first))) {
    snprintf(reply, size, "%s", usage);
    return Invalid;
  }
  request.paced = strcmp(first, "--continuous") != 0;
  if ((!request.paced && !word(text, first, sizeof(first))) ||
      !word(text, second, sizeof(second)) ||
      !word(text, extra, sizeof(extra)) || *extra) {
    snprintf(reply, size, "%s", usage);
    return Invalid;
  }
  if (request.paced && !*second && strcmp(first, "status") == 0) return Status;
  if (request.paced && !*second && strcmp(first, "stop") == 0) return Stop;
  unsigned int seconds = 0;
  for (const char* p = first; *p; ++p) {
    if (*p < '0' || *p > '9') { seconds = 0; break; }
    seconds = seconds * 10 + *p - '0';
    if (seconds > MaxSeconds) { seconds = 0; break; }
  }
  if (!*first || !seconds || seconds > MaxSeconds) {
    snprintf(reply, size, "%s", usage);
    return Invalid;
  }
  strcpy(request.region, second);
  request.seconds = seconds;
  return Start;
}

bool FloodTest::start(uint32_t now, uint32_t seconds, uint32_t run_id, bool paced) {
  if (busy() || !seconds || seconds > MaxSeconds) return false;
  _start = _end = now;
  _duration = seconds * 1000u;
  _run_id = run_id;
  _paced = paced;
  _pause_ms = 0;
  _queued = _sent = _failed = 0;
  _active = _report = true;
  return true;
}

bool FloodTest::expire(uint32_t now, mesh::PacketManager& packets) {
  if (!_active || now - _start < _duration) return false;
  stop(now, packets);
  return true;
}

void FloodTest::stop(uint32_t now, mesh::PacketManager& packets) {
  if (!busy()) return;
  _active = false;
  _end = now;
  for (int i = 0; _pending && i < packets.getOutboundTotal(); ++i) {
    if (packets.getOutboundByIdx(i) == _pending) {
      packets.free(packets.removeOutboundByIdx(i));
      _pending = nullptr;
      break;
    }
  }
}

void FloodTest::finished(const mesh::Packet* packet, bool success, uint32_t now, uint32_t airtime_ms) {
  if (packet != _pending) return;
  _pending = nullptr;
  if (success) {
    ++_sent;
    _last_tx = now;
    _pause_ms = _paced ? airtime_ms : 0;
  } else {
    ++_failed;
  }
  if (!_active) _end = now;
}

bool FloodTest::takeReport() {
  if (!_report || busy()) return false;
  _report = false;
  return true;
}

void FloodTest::payload(uint8_t* dest, size_t size) const {
  for (size_t i = 0; i < size; ++i) dest[i] = static_cast<uint8_t>(i ^ 0xA5);
  const char tag[] = "MC-DUTY";
  if (size < sizeof(tag) - 1 + 8) return;
  memcpy(dest, tag, sizeof(tag) - 1);
  for (unsigned int i = 0; i < 4; ++i) {
    dest[7 + i] = _run_id >> (i * 8);
    dest[11 + i] = _queued >> (i * 8);
  }
}
