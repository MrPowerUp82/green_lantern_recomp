#include "guest_thread.h"
#include "memory.h"
#include "../recompiled/ppc_recomp_shared.h"
#include <atomic>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <thread>
#endif

namespace Kernel {
    static std::atomic<bool> s_running{false};

#if defined(_WIN32)
    static HANDLE s_thread = nullptr;
#else
    static std::thread s_thread;
#endif

    static void RunMain() {
        uint8_t* base = MemoryManager::GetBase();

        // PCR/TLS do Xenon: r13 aponta para o bloco por-thread (PCR) alocado no heap do guest
        GuestAddr pcr = MemoryManager::AllocateVirtual(GuestThread::kPcrSize);
        std::memset(base + pcr, 0, GuestThread::kPcrSize);
        // PCR+0x70 = topo da stack, PCR+0x74 = base... (campos preenchidos conforme o jogo exigir)
        Endian::WriteBe32(base + pcr + 0x70, GuestThread::kStackBase + GuestThread::kStackSize);
        Endian::WriteBe32(base + pcr + 0x74, GuestThread::kStackBase);

        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        ctx.r1.u64 = GuestThread::kStackBase + GuestThread::kStackSize - 0x100; // stack pointer (alinhado a 16)
        ctx.r13.u64 = pcr;
        ctx.lr = 0;

        std::cout << "[Guest] Thread principal: r1=0x" << std::hex << ctx.r1.u32
                  << " r13=0x" << ctx.r13.u32 << " entry=0x" << GuestThread::kEntryPoint
                  << std::dec << std::endl;

        // _xstart e o nome que o XenonRecomp da ao entry point 0x822FC750
        _xstart(ctx, base);

        std::cout << "[Guest] Entry point retornou (r3=" << ctx.r3.s32 << ")." << std::endl;
    }

#if defined(_WIN32)
    static DWORD WINAPI ThreadProc(LPVOID) {
        RunMain();
        s_running = false;
        return 0;
    }
#endif

    bool GuestThread::StartMain() {
        if (!MemoryManager::GetBase()) return false;
        s_running = true;
#if defined(_WIN32)
        // 64 MB de stack host: o codigo recompilado encadeia chamadas nativas profundas
        s_thread = CreateThread(nullptr, 64u * 1024 * 1024, ThreadProc, nullptr,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!s_thread) {
            s_running = false;
            std::cerr << "[Guest] Falha ao criar a thread principal." << std::endl;
            return false;
        }
#else
        s_thread = std::thread([] { RunMain(); s_running = false; });
#endif
        return true;
    }

    bool GuestThread::IsRunning() { return s_running; }

    bool GuestThread::Join(uint32_t timeoutMs) {
#if defined(_WIN32)
        if (!s_thread) return true;
        return WaitForSingleObject(s_thread, timeoutMs) == WAIT_OBJECT_0;
#else
        (void)timeoutMs;
        if (s_thread.joinable()) s_thread.join();
        return true;
#endif
    }
}
