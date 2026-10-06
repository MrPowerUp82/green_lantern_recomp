#pragma once

#include "types.h"
#include <cstdint>

namespace Gpu {
    // Codigo de endian nos 2 bits baixos de enderecos de escrita/leitura da GPU.
    enum class SwapMode : uint32_t {
        None = 0,
        k8in16 = 1,
        k8in32 = 2,
        k16in32 = 3,
    };

    inline uint32_t GpuSwap(uint32_t value, SwapMode mode) {
        switch (mode) {
        case SwapMode::k8in16:
            return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
        case SwapMode::k8in32:
            return ::Endian::Swap32(value);
        case SwapMode::k16in32:
            return (value >> 16) | (value << 16);
        default:
            return value;
        }
    }

    // Acesso ao espaco de enderecos do guest (memoria big-endian, base + endereco).
    // As leituras e escritas sao volatile: o guest e outras threads modificam a mesma memoria.
    class GuestMemory {
    public:
        explicit GuestMemory(uint8_t* base = nullptr) : m_base(base) {}

        uint8_t* Base() const { return m_base; }

        // Endereco nao nulo e dentro do espaco de 4 GB.
        static bool Valid(GuestAddr addr, uint64_t sizeBytes) {
            return addr != 0 && uint64_t(addr) + sizeBytes <= 0x100000000ull;
        }

        uint32_t LoadRaw32(GuestAddr addr) const {
            return *reinterpret_cast<const volatile uint32_t*>(m_base + addr);
        }
        void StoreRaw32(GuestAddr addr, uint32_t value) const {
            *reinterpret_cast<volatile uint32_t*>(m_base + addr) = value;
        }
        uint32_t LoadBe32(GuestAddr addr) const { return ::Endian::Swap32(LoadRaw32(addr)); }
        void StoreBe32(GuestAddr addr, uint32_t value) const { StoreRaw32(addr, ::Endian::Swap32(value)); }

        void StoreBe16(GuestAddr addr, uint16_t value) const {
            m_base[addr] = uint8_t(value >> 8);
            m_base[addr + 1] = uint8_t(value);
        }

    private:
        uint8_t* m_base;
    };
}
