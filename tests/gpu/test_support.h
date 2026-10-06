#pragma once

#include "gpu/gpu_registers.h"
#include "gpu/guest_memory.h"
#include "gpu/pm4.h"
#include "test_harness.h"

#include <cstring>
#include <initializer_list>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace TestSupport {
    constexpr GuestAddr kHeapBase = 0x40000000;
    constexpr uint32_t kHeapSize = 0x04000000;  // 64 MB
    constexpr GuestAddr kRingBase = 0x40000000;
    constexpr GuestAddr kScratchBase = 0x41000000;

    // Espaco de 4 GB do guest, reservado uma vez; so o heap e a pagina MMIO sao commitados.
    inline uint8_t* GuestBase() {
        static uint8_t* base = [] {
#if defined(_WIN32)
            uint8_t* b = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE));
            if (!b || !VirtualAlloc(b + kHeapBase, kHeapSize, MEM_COMMIT, PAGE_READWRITE) ||
                !VirtualAlloc(b + Gpu::Reg::kMmioBase, Gpu::Reg::kMmioSize, MEM_COMMIT, PAGE_READWRITE)) {
                std::fprintf(stderr, "Falha ao reservar memoria dos testes.\n");
                std::abort();
            }
#else
            void* reservation = mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            uint8_t* b = static_cast<uint8_t*>(reservation);
            if (reservation == MAP_FAILED ||
                mprotect(b + kHeapBase, kHeapSize, PROT_READ | PROT_WRITE) != 0 ||
                mprotect(b + Gpu::Reg::kMmioBase, Gpu::Reg::kMmioSize, PROT_READ | PROT_WRITE) != 0) {
                std::fprintf(stderr, "Falha ao reservar memoria dos testes.\n");
                std::abort();
            }
#endif
            return b;
        }();
        return base;
    }

    // Alocador bump de 4 KB para o selftest e os testes de Vd*.
    inline GuestAddr Allocate(uint32_t size) {
        static GuestAddr cursor = kScratchBase;
        const GuestAddr result = cursor;
        cursor += (size + 0xFFF) & ~0xFFFu;
        return result;
    }

    // Escreve dwords big-endian numa area nova do guest (indirect buffers, constantes) e devolve o endereco.
    inline GuestAddr WriteGuestDwords(std::initializer_list<uint32_t> words) {
        const GuestAddr addr = Allocate(0x1000);
        const Gpu::GuestMemory mem(GuestBase());
        uint32_t i = 0;
        for (uint32_t w : words) mem.StoreBe32(addr + 4 * i++, w);
        return addr;
    }
}
