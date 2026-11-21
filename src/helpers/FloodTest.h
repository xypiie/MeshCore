#pragma once

#include <Dispatcher.h>

// A bounded producer for lab traffic. Scheduling and airtime limiting remain
// entirely in the original dispatcher; at most one test packet is outstanding.
class FloodTest {
public:
  enum Command { NotHandled, Invalid, Start, Stop, Status };
  struct Request {
    char region[31];
    uint32_t seconds;
    bool paced;
  };
  static constexpr uint32_t MaxSeconds = 7200;
  static Command parse(const char* command, bool local, Request& request,
                       char* reply, size_t reply_size);

private:
  bool _active = false, _report = false;
  uint32_t _start = 0, _end = 0, _duration = 0, _run_id = 0;
  uint32_t _queued = 0, _sent = 0, _failed = 0;
  bool _paced = false;
  uint32_t _last_tx = 0, _pause_ms = 0;
  mesh::Packet* _pending = nullptr;

public:
  bool start(uint32_t now, uint32_t seconds, uint32_t run_id, bool paced = true);
  bool expire(uint32_t now, mesh::PacketManager& packets);
  void stop(uint32_t now, mesh::PacketManager& packets);
  void finished(const mesh::Packet* packet, bool success, uint32_t now, uint32_t airtime_ms = 0);
  bool canQueue(uint32_t now) const {
    return _active && !_pending && (!_pause_ms || now - _last_tx >= _pause_ms);
  }
  bool owns(const mesh::Packet* packet) const { return _pending == packet; }
  bool busy() const { return _active || _pending; }
  bool active() const { return _active; }
  bool takeReport();
  void payload(uint8_t* dest, size_t size) const;
  void queued(mesh::Packet* packet) { _pending = packet; ++_queued; }
  uint32_t elapsed(uint32_t now) const {
    return (busy() ? now : _end) - _start;
  }
  uint32_t duration() const { return _duration; }
  uint32_t sent() const { return _sent; }
  uint32_t failed() const { return _failed; }
  uint32_t generated() const { return _queued; }
};
