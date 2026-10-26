// Shim minimo de xenia/base/math.h para os headers do Xenia portados em src/gpu/xenos.
#pragma once

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "xenia/base/assert.h"
#include "xenia/base/platform.h"

namespace xe {
    using fourcc_t = uint32_t;
    constexpr fourcc_t make_fourcc(const char (&s)[5]) {
        return (fourcc_t(uint8_t(s[0])) << 24) | (fourcc_t(uint8_t(s[1])) << 16) |
               (fourcc_t(uint8_t(s[2])) << 8) | fourcc_t(uint8_t(s[3]));
    }

    // O host e little-endian: le<T> e o proprio valor.
    template <typename T>
    using le = T;

    template <typename T, size_t N>
    constexpr size_t countof(T (&)[N]) { return N; }

    template <typename T>
    constexpr T align(T value, T alignment) { return (value + alignment - 1) & ~(alignment - 1); }

    template <typename T>
    constexpr T round_up(T value, T multiple, bool force_non_zero = true) {
        if (force_non_zero && !value) return multiple;
        return (value + multiple - 1) / multiple * multiple;
    }

    template <typename T>
    constexpr T next_pow2(T value) {
        using U = std::make_unsigned_t<T>;
        return value <= 1 ? T(1) : T(std::bit_ceil(U(value)));
    }

    template <typename T>
    constexpr uint8_t lzcnt(T v) { return uint8_t(std::countl_zero(std::make_unsigned_t<T>(v))); }
    template <typename T>
    constexpr uint8_t tzcnt(T v) { return uint8_t(std::countr_zero(std::make_unsigned_t<T>(v))); }

    template <typename T>
    constexpr T log2_floor(T v) { return T(sizeof(T) * 8 - 1 - lzcnt(v)); }
    template <typename T>
    constexpr T log2_ceil(T v) { return T(sizeof(T) * 8 - lzcnt(T(v - 1))); }

    template <typename T>
    constexpr uint32_t bit_count(T v) { return uint32_t(std::popcount(std::make_unsigned_t<T>(v))); }

    template <typename T>
    constexpr T clamp_float(T value, T min_value, T max_value) {
        return std::max(min_value, std::min(max_value, value));
    }
    template <typename T>
    constexpr T saturate(T value) { return clamp_float(value, T(0), T(1)); }

    inline uint16_t byte_swap(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
    inline uint32_t byte_swap(uint32_t v) {
        return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    }
    inline uint64_t byte_swap(uint64_t v) {
        return (uint64_t(byte_swap(uint32_t(v))) << 32) | byte_swap(uint32_t(v >> 32));
    }
}
