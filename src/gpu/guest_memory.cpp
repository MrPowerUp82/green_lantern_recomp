#include "gpu/guest_memory.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Gpu {
    bool GuestMemory::IsReadable(GuestAddr addr, uint64_t sizeBytes) const {
        if (!m_base || !Valid(addr, sizeBytes)) return false;
        if (sizeBytes == 0) return true;
#if defined(_WIN32)
        // Ultima regiao commitada vista nesta thread: a maioria das consultas cai nela.
        thread_local const uint8_t* cachedLo = nullptr;
        thread_local const uint8_t* cachedHi = nullptr;
        const uint8_t* lo = m_base + addr;
        const uint8_t* hi = lo + sizeBytes;
        if (lo >= cachedLo && hi <= cachedHi) return true;
        const uint8_t* p = lo;
        while (p < hi) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || info.Protect == 0) {
                return false;
            }
            const uint8_t* regionEnd = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
            if (p == lo) {
                cachedLo = static_cast<const uint8_t*>(info.BaseAddress);
                cachedHi = regionEnd;
            }
            p = regionEnd;
        }
        return true;
#else
        return true;
#endif
    }
}
