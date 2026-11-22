#pragma once

#include <stdint.h>
#include <string.h>

namespace mesh {

// Count a completed transmission in the bucket containing its completion.
// Retaining the entire oldest overlapping bucket makes the result conservative.
class TxAirtimeWindow {
  static constexpr uint32_t BucketCount = 61;
  struct Bucket {
    uint32_t total, burst;
  };
  Bucket _buckets[BucketCount]{};
  uint32_t _bucket_start = 0, _width = 1, _current = 0, _used = 0, _burst_used = 0;

public:
  void begin(uint32_t now, uint32_t window_ms) {
    memset(_buckets, 0, sizeof(_buckets));
    _bucket_start = now;
    _width = (window_ms + BucketCount - 2) / (BucketCount - 1);
    if (_width == 0) _width = 1;
    _current = _used = _burst_used = 0;
  }

  void advance(uint32_t now) {
    const uint32_t steps = (now - _bucket_start) / _width;
    if (!steps) return;
    _bucket_start += steps * _width;
    if (steps >= BucketCount) {
      memset(_buckets, 0, sizeof(_buckets));
      _current = _used = _burst_used = 0;
      return;
    }
    for (uint32_t i = 0; i < steps; ++i) {
      _current = (_current + 1) % BucketCount;
      _used -= _buckets[_current].total;
      _burst_used -= _buckets[_current].burst;
      _buckets[_current] = {};
    }
  }

  void record(uint32_t duration_ms, bool burst = false) {
    _buckets[_current].total += duration_ms;
    _used += duration_ms;
    if (burst) {
      _buckets[_current].burst += duration_ms;
      _burst_used += duration_ms;
    }
  }

  uint32_t used() const { return _used; }
  uint32_t burstUsed() const { return _burst_used; }
};

}  // namespace mesh
