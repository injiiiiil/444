/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/Portability.h>
#include <folly/algorithm/simd/Movemask.h>
#include <folly/algorithm/simd/detail/SimdForEach.h>
#include <folly/lang/Bits.h>

#include <array>

#if FOLLY_X64
#include <immintrin.h>
#endif

#if FOLLY_AARCH64
#include <arm_neon.h>
#endif

namespace folly {
namespace simd::detail {

/**
 * SimdCharPlatform
 *
 * Common interface for some SIMD operations on chars between: sse2, avx2,
 * arm-neon. (maybe we will move to sse4.2 at some point, we don't care much for
 * pure sse2).
 *
 * If it's not one of the supported platforms, std::same_as<SimdCharPlatform,
 * void>.
 * There is also a macro: FOLLY_DETAIL_HAS_SIMD_CHAR_PLATFORM set to 1 or 0
 *
 * Nested types:
 * - reg_t - type of a simd register (__m128i)
 * - logical_t - type of a simd logical register (matches reg_t so far)
 * - mmask_t - type of an integer bitmask we can get from logical (similar to
 *             _mm_movemask_epi8).
 *
 * Nested constants:
 *  - kCardinal - number of elements in a register
 *  - kMmaskBitsPerElement - number of bits per element in a mmask_t
 *
 * loads:
 *  - loadu(const char*, ignore_none)
 *  - unsafeLoadU(const char*, ignore_none)
 *  - loada(const char*, ignore)
 *
 * a/u stand for aligned/unaligned. Ignored values can be garbage. unsafe
 * disables sanitizers.
 *
 * reg ops:
 *  - equal(reg_t, char) - by lane comparison against a char.
 *  - le_unsigned(reg_t, char) - by lane less than or equal to char.
 *
 * logical ops:
 *   - movemask - take a bitmask
 *   - any(logical_t, ignore) - return true if any the lanes are true
 *   - logical_or(logical_t, logical_t) - by lane logical or
 *
 * mmask ops:
 *   - clear(mmask, ignore) - sets ignored bits to 0
 *
 */

#if FOLLY_X64 || FOLLY_AARCH64

template <typename Platform>
struct SimdCharPlatformCommon : Platform {
  using logical_t = typename Platform::logical_t;
  using movemask_result_t =
      decltype(folly::movemask<std::uint8_t>(logical_t{}));
  using mmask_t = typename movemask_result_t::first_type;
  static constexpr std::uint32_t kMmaskBitsPerElement =
      typename movemask_result_t::second_type{}();

  template <typename Uint>
  FOLLY_NODISCARD FOLLY_ALWAYS_INLINE static Uint setLowerNBits(int n) {
    if (sizeof(Uint) == 8 && n == 64) {
      return static_cast<Uint>(-1);
    }
    return static_cast<Uint>((std::uint64_t{1} << n) - 1);
  }

  FOLLY_NODISCARD FOLLY_ALWAYS_INLINE static mmask_t clear(
      mmask_t mmask, ignore_extrema ignore) {
    mmask_t clearFirst =
        ~setLowerNBits<mmask_t>(ignore.first * kMmaskBitsPerElement);
    mmask_t clearLast = setLowerNBits<mmask_t>(
        (Platform::kCardinal - ignore.last) * kMmaskBitsPerElement);
    return mmask & clearFirst & clearLast;
  }

  FOLLY_NODISCARD FOLLY_ALWAYS_INLINE static mmask_t clear(
      mmask_t mmask, ignore_none) {
    return mmask;
  }

  // These are aligned loads but there is no point in generating
  // aligned load instructions, so we call loadu.
  FOLLY_ALWAYS_INLINE
  static auto loada(const char* ptr, ignore_none) {
    return Platform::loadu(ptr, ignore_none{});
  }

  FOLLY_ALWAYS_INLINE
  static auto loada(const char* ptr, ignore_extrema) {
    return Platform::unsafeLoadu(ptr, ignore_none{});
  }

  FOLLY_ALWAYS_INLINE
  static mmask_t movemask(logical_t log) {
    return folly::movemask<std::uint8_t>(log).first;
  }

  using Platform::any;

  FOLLY_ALWAYS_INLINE
  static bool any(typename Platform::logical_t log, ignore_extrema ignore) {
    auto mmask = movemask(log);
    mmask = clear(mmask, ignore);
    return mmask;
  }

  static auto toArray(typename Platform::reg_t x) {
    std::array<std::uint8_t, Platform::kCardinal> buf;
    std::memcpy(buf.data(), &x, Platform::kCardinal);
    return buf;
  }
};

#endif

#if FOLLY_X64

struct SimdCharSse2PlatformSpecific {
  using reg_t = __m128i;
  using logical_t = reg_t;

  static constexpr int kCardinal = 16;

  // Even for aligned loads intel people don't recommend using
  // aligned load instruction
  FOLLY_ALWAYS_INLINE
  static reg_t loadu(const char* p, ignore_none) {
    return _mm_loadu_si128(reinterpret_cast<const reg_t*>(p));
  }

  FOLLY_DISABLE_SANITIZERS
  FOLLY_ALWAYS_INLINE
  static reg_t unsafeLoadu(const char* p, ignore_none) {
    return _mm_loadu_si128(reinterpret_cast<const reg_t*>(p));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t equal(reg_t reg, char x) {
    return _mm_cmpeq_epi8(reg, _mm_set1_epi8(x));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t le_unsigned(reg_t reg, char x) {
    // No unsigned comparisons on x86
    // less equal <=> equal (min)
    reg_t min = _mm_min_epu8(reg, _mm_set1_epi8(x));
    return _mm_cmpeq_epi8(reg, min);
  }

  FOLLY_ALWAYS_INLINE
  static logical_t logical_or(logical_t x, logical_t y) {
    return _mm_or_si128(x, y);
  }

  FOLLY_ALWAYS_INLINE
  static bool any(logical_t log, ignore_none) {
    return folly::movemask<std::uint8_t>(log).first;
  }
};

#define FOLLY_DETAIL_HAS_SIMD_CHAR_PLATFORM 1

using SimdCharSse2Platform =
    SimdCharPlatformCommon<SimdCharSse2PlatformSpecific>;

#if defined(__AVX2__)

struct SimdCharAvx2PlatformSpecific {
  using reg_t = __m256i;
  using logical_t = reg_t;

  static constexpr int kCardinal = 32;

  // We can actually use aligned loads but our Intel people don't recommend
  FOLLY_ALWAYS_INLINE
  static reg_t loadu(const char* p, ignore_none) {
    return _mm256_loadu_si256(reinterpret_cast<const reg_t*>(p));
  }

  FOLLY_DISABLE_SANITIZERS
  FOLLY_ALWAYS_INLINE
  static reg_t unsafeLoadu(const char* p, ignore_none) {
    return _mm256_loadu_si256(reinterpret_cast<const reg_t*>(p));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t equal(reg_t reg, char x) {
    return _mm256_cmpeq_epi8(reg, _mm256_set1_epi8(x));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t le_unsigned(reg_t reg, char x) {
    // See SSE comment
    reg_t min = _mm256_min_epu8(reg, _mm256_set1_epi8(x));
    return _mm256_cmpeq_epi8(reg, min);
  }

  FOLLY_ALWAYS_INLINE
  static logical_t logical_or(logical_t x, logical_t y) {
    return _mm256_or_si256(x, y);
  }

  FOLLY_ALWAYS_INLINE
  static bool any(logical_t log, ignore_none) {
    return folly::movemask<std::uint8_t>(log).first;
  }
};

using SimdCharAvx2Platform =
    SimdCharPlatformCommon<SimdCharAvx2PlatformSpecific>;

using SimdCharPlatform = SimdCharAvx2Platform;

#else
using SimdCharPlatform = SimdCharSse2Platform;
#endif

#elif FOLLY_AARCH64

struct SimdCharAarch64PlatformSpecific {
  using reg_t = uint8x16_t;
  using logical_t = reg_t;

  static constexpr int kCardinal = 16;

  FOLLY_ALWAYS_INLINE
  static reg_t loadu(const char* p, ignore_none) {
    return vld1q_u8(reinterpret_cast<const std::uint8_t*>(p));
  }

  FOLLY_DISABLE_SANITIZERS
  FOLLY_ALWAYS_INLINE
  static reg_t unsafeLoadu(const char* p, ignore_none) {
    return vld1q_u8(reinterpret_cast<const std::uint8_t*>(p));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t equal(reg_t reg, char x) {
    return vceqq_u8(reg, vdupq_n_u8(static_cast<std::uint8_t>(x)));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t le_unsigned(reg_t reg, char x) {
    return vcleq_u8(reg, vdupq_n_u8(static_cast<std::uint8_t>(x)));
  }

  FOLLY_ALWAYS_INLINE
  static logical_t logical_or(logical_t x, logical_t y) {
    return vorrq_u8(x, y);
  }

  FOLLY_ALWAYS_INLINE
  static bool any(logical_t log, ignore_none) { return vmaxvq_u8(log); }
};

#define FOLLY_DETAIL_HAS_SIMD_CHAR_PLATFORM 1

using SimdCharAarch64Platform =
    SimdCharPlatformCommon<SimdCharAarch64PlatformSpecific>;

using SimdCharPlatform = SimdCharAarch64Platform;

#define FOLLY_DETAIL_HAS_SIMD_CHAR_PLATFORM 1

#else

#define FOLLY_DETAIL_HAS_SIMD_CHAR_PLATFORM 0

using SimdCharPlatform = void;

#endif

} // namespace simd::detail
} // namespace folly
