#pragma once

#include <cstdint>
#include <cstddef>
#include <bit>
#include <string>

// Tipos comuns da arquitetura de 32 bits do Xbox 360 (Xenon)
using GuestAddr = uint32_t;
using GuestSize = uint32_t;

// Utilidades para conversão de Endianness (PowerPC é Big-Endian, x86_64 é Little-Endian)
namespace Endian {
    inline uint16_t Swap16(uint16_t val) {
#if defined(_MSC_VER)
        return _byteswap_ushort(val);
#else
        return __builtin_bswap16(val);
#endif
    }

    inline uint32_t Swap32(uint32_t val) {
#if defined(_MSC_VER)
        return _byteswap_ulong(val);
#else
        return __builtin_bswap32(val);
#endif
    }

    inline uint64_t Swap64(uint64_t val) {
#if defined(_MSC_VER)
        return _byteswap_uint64(val);
#else
        return __builtin_bswap64(val);
#endif
    }

    // Leitura direta com troca de endian
    inline uint32_t ReadBe32(const void* ptr) {
        uint32_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return Swap32(val);
    }

    inline void WriteBe32(void* ptr, uint32_t val) {
        uint32_t swapped = Swap32(val);
        std::memcpy(ptr, &swapped, sizeof(swapped));
    }
}
