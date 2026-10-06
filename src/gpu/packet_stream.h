#pragma once

#include "gpu/guest_memory.h"
#include <cstdint>

namespace Gpu {
    // Leitor de dwords PM4 sobre memoria do guest. Serve para o ring (com wraparound) e para
    // indirect buffers (capacidade == quantidade de dwords, logo nunca da a volta).
    class PacketStream {
    public:
        PacketStream(const GuestMemory& memory, GuestAddr start, uint32_t capacityDwords,
                     uint32_t readIndex, uint32_t remainingDwords)
            : m_memory(memory), m_start(start), m_capacity(capacityDwords),
              m_readIndex(readIndex), m_remaining(remainingDwords) {}

        uint32_t Remaining() const { return m_remaining; }
        uint32_t ReadIndex() const { return m_readIndex; }

        // Le um dword (big-endian -> host) e avanca. false se acabou ou o endereco e invalido.
        bool Read(uint32_t* out) {
            if (!out || !m_memory.Base() || m_remaining == 0 || m_capacity == 0 ||
                m_readIndex >= m_capacity || m_remaining > m_capacity || (m_start & 3)) return false;
            const uint64_t addr = uint64_t(m_start) + uint64_t(m_readIndex) * 4;
            if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
            *out = m_memory.LoadBe32(GuestAddr(addr));
            m_readIndex = (m_readIndex + 1 >= m_capacity) ? 0 : m_readIndex + 1;
            --m_remaining;
            return true;
        }

    private:
        GuestMemory m_memory;
        GuestAddr m_start;
        uint32_t m_capacity;
        uint32_t m_readIndex;
        uint32_t m_remaining;
    };
}
