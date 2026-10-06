#include "gpu/selftest.h"

#include "recompiled/ppc_context.h"

#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSwap);

namespace Gpu {
    bool RunSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel) {
        GpuSystem& gpu = GpuSystem::Get();
        if (!gpu.IsInitialized()) return false;
        const GuestMemory& mem = gpu.Memory();
        uint8_t* base = mem.Base();

        constexpr uint32_t kRingSizeLog2 = 13;                      // 1 << (13 + 3) = 64 KB
        constexpr uint32_t kRingBytes = 1u << (kRingSizeLog2 + 3);
        constexpr uint32_t kRingDwords = kRingBytes / 4;
        constexpr uint32_t kSwapDwords = 64;                        // reserva de VdSwap no ring
        constexpr uint32_t kWidth = 1280;
        constexpr uint32_t kHeight = 720;

        // scratch: +0x000 write-back do RPTR, +0x010 fetch constant, +0x030 ptr do frontbuffer,
        //          +0x034 largura, +0x038 altura, +0x800 topo da pilha fake (r1)
        const GuestAddr ring = gpu.Allocate(kRingBytes);
        const GuestAddr scratch = gpu.Allocate(0x1000);
        const GuestAddr frontbuffer = gpu.Allocate(0x1000);
        if (!ring || !scratch || !frontbuffer) {
            std::cerr << "[SelfTest] Falha ao alocar memoria do guest." << std::endl;
            return false;
        }
        std::memset(base + ring, 0, kRingBytes);
        std::memset(base + scratch, 0, 0x1000);

        const GuestAddr writeback = scratch + 0x000;
        const GuestAddr fetch = scratch + 0x010;
        const GuestAddr frontbufferPtr = scratch + 0x030;
        const GuestAddr widthPtr = scratch + 0x034;
        const GuestAddr heightPtr = scratch + 0x038;
        const GuestAddr stack = scratch + 0x800;

        mem.StoreBe32(fetch + 4, frontbuffer);                        // dword_1: base_address << 12
        mem.StoreBe32(fetch + 8, (kWidth - 1) | ((kHeight - 1) << 13)); // dword_2: size_2d
        mem.StoreBe32(frontbufferPtr, frontbuffer);
        mem.StoreBe32(widthPtr, kWidth);
        mem.StoreBe32(heightPtr, kHeight);
        mem.StoreBe32(stack + 0x54, widthPtr);
        mem.StoreBe32(stack + 0x5C, heightPtr);

        PPCContext ctx{};
        ctx.r1.u64 = stack;

        ctx.r3.u64 = ring;
        ctx.r4.u64 = kRingSizeLog2;
        __imp__VdInitializeRingBuffer(ctx, base);

        ctx.r3.u64 = writeback;
        ctx.r4.u64 = 6;
        __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);

        uint32_t writeIndex = 0;
        uint32_t submittedFrames = 0;
        const uint64_t swapsBefore = gpu.Cp().Stats().swaps;
        for (uint32_t frame = 0; frame < frames; frame++) {
            if (cancel && cancel->load()) break;
            // Nunca sobrescrever comandos ainda pendentes nem publicar WPTR == RPTR com ring cheio.
            const auto spaceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while ((mem.LoadBe32(writeback) + kRingDwords - writeIndex - 1) % kRingDwords < kSwapDwords) {
                if (cancel && cancel->load()) break;
                if (std::chrono::steady_clock::now() > spaceDeadline) {
                    std::cerr << "[SelfTest] Ring sem espaco a tempo." << std::endl;
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (cancel && cancel->load()) break;
            ctx.r3.u64 = ring + writeIndex * 4;
            ctx.r4.u64 = fetch;
            ctx.r8.u64 = frontbufferPtr;
            __imp__VdSwap(ctx, base);

            writeIndex = (writeIndex + kSwapDwords) % kRingDwords;
            std::atomic_thread_fence(std::memory_order_release);
            mem.StoreBe32(Reg::MmioAddress(Reg::kCpRbWptr), writeIndex);
            ++submittedFrames;

            std::this_thread::sleep_for(std::chrono::milliseconds(frameIntervalMs));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (mem.LoadBe32(writeback) != writeIndex) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "[SelfTest] CP nao consumiu o ring a tempo." << std::endl;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return gpu.Cp().Stats().swaps - swapsBefore == submittedFrames;
    }
}
