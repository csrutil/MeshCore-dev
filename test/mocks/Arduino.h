#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include "Stream.h"

inline uint32_t g_mock_millis = 0;

using std::isnan;
using std::abs;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}

// helpers used by firmware code compiled into the native tests
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
inline char* ltoa(long value, char* str, int base) {
  (void)base;
  sprintf(str, "%ld", value);
  return str;
}
