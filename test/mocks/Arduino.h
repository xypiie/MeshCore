#pragma once

#include <cstdint>
#include <cmath>
#include "Stream.h"

#ifdef MESH_TEST_REGION_MAP
#include "MemoryFileSystem.h"
#define FILESYSTEM MemoryFileSystem
inline char* ltoa(long value, char* text, int base) {
  if (base == 16) std::snprintf(text, 32, "%lx", static_cast<unsigned long>(value));
  else std::snprintf(text, 32, "%ld", value);
  return text;
}
#endif

inline uint32_t g_mock_millis = 0;

using std::isnan;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}

inline uint32_t micros() { return g_mock_millis * 1000; }
