// Shim de xenia/base/xxhash.h: FNV-1a 64 (so usado para hash de structs pequenas).
#pragma once

#include <cstddef>
#include <cstdint>

inline uint64_t XXH3_64bits(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
