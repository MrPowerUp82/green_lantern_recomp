#pragma once

#include "gpu/command_processor.h"
#include "gpu/gpu_backend.h"
#include "test_support.h"

#include <vector>

namespace TestSupport {
    struct RecordingSink final : Gpu::IInterruptSink {
        struct Call { uint32_t source; uint32_t cpu; };
        void Dispatch(uint32_t source, uint32_t cpu) override { calls.push_back({ source, cpu }); }
        std::vector<Call> calls;
    };

    // Ring de teste: o "D3D" escreve dwords com Emit() e publica o WPTR com Submit().
    struct CpRig {
        explicit CpRig(uint32_t sizeLog2 = 8)
            : mem(GuestBase()), cp(GuestBase(), &backend, &sink) {
            ringDwords = (1u << (sizeLog2 + 3)) / 4;
            std::memset(GuestBase() + kRingBase, 0, ringDwords * 4);
            mem.StoreBe32(Gpu::Reg::MmioAddress(Gpu::Reg::kCpRbWptr), 0);
            cp.InitializeRingBuffer(kRingBase, sizeLog2);
        }

        void Emit(std::initializer_list<uint32_t> words) {
            for (uint32_t w : words) {
                mem.StoreBe32(kRingBase + wptr * 4, w);
                wptr = (wptr + 1) % ringDwords;
            }
        }

        // Publica o WPTR na pagina MMIO (o que o D3D faz depois de escrever os pacotes).
        void Publish() { mem.StoreBe32(Gpu::Reg::MmioAddress(Gpu::Reg::kCpRbWptr), wptr); }

        // Publica o WPTR e deixa o CP processar (sincrono, sem thread).
        bool Submit() {
            Publish();
            return cp.PumpOnce();
        }

        Gpu::GuestMemory mem;
        Gpu::NullBackend backend;
        RecordingSink sink;
        Gpu::CommandProcessor cp;
        uint32_t ringDwords = 0;
        uint32_t wptr = 0;
    };
}
