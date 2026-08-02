#pragma once

#include <cstdlib>
#include <cstring>

#include "common/common_types.h"

u32 crc32(const u8* data, size_t size);

// Only select intrinsics when the compiler target guarantees the required instructions.
#if !defined(OPENGOAL_FORCE_SOFTWARE_CRC32C) && defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define OPENGOAL_HAS_ARM_CRC32C 1
#elif !defined(OPENGOAL_FORCE_SOFTWARE_CRC32C) && defined(__SSE4_2__)
#include <nmmintrin.h>
#define OPENGOAL_HAS_X86_CRC32C 1
#endif

// Computes CRC32C
inline u32 crc32(const u8* data, size_t size) {
  u32 result = 0xffffffff;
#if defined(OPENGOAL_HAS_ARM_CRC32C)
  while (size >= 4) {
    u32 word;
    std::memcpy(&word, data, sizeof(word));
    result = __crc32cw(result, word);
    data += 4;
    size -= 4;
  }
  while (size) {
    result = __crc32cb(result, *data);
    data++;
    size--;
  }
#elif defined(OPENGOAL_HAS_X86_CRC32C)
  while (size >= 4) {
    u32 word;
    std::memcpy(&word, data, sizeof(word));
    data += 4;
    size -= 4;
    result = _mm_crc32_u32(result, word);
  }
  while (size) {
    result = _mm_crc32_u8(result, *data);
    data++;
    size--;
  }
#else
  while (size--) {
    result ^= *data++;
    for (int bit = 0; bit < 8; ++bit) {
      result = (result & 1) ? (result >> 1) ^ 0x82f63b78 : result >> 1;
    }
  }
#endif
  return ~result;
}

#undef OPENGOAL_HAS_ARM_CRC32C
#undef OPENGOAL_HAS_X86_CRC32C
