#include "crash_handler.h"

#include "recompiled/ppc_recomp_shared.h"
#include "types.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Platform {

#if defined(_WIN32)
    namespace {
        struct Entry {
            uintptr_t host;
            uint32_t guest;
        };

        std::vector<Entry> g_entries;
        uint8_t* g_guestBase = nullptr;
        volatile LONG g_reporting = 0;
        thread_local PPCContext* t_context = nullptr;
        struct SampledThread {
            HANDLE handle;
            DWORD id;
            PPCContext* context;
        };
        std::mutex g_threadsLock;
        std::vector<SampledThread> g_threads;

        uint32_t GuestLoad(uint32_t addr) {
            if (!g_guestBase || addr < 0x10000) return 0;
            return Endian::ReadBe32(g_guestBase + addr);
        }

        void PrintGuestStateFor(const PPCContext* contextPtr) {
            if (!contextPtr) return;
            const PPCContext& c = *contextPtr;
            std::fprintf(stderr, "[Crash] guest: lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X r31=%08X\n",
                         static_cast<uint32_t>(c.lr), c.ctr.u32, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32,
                         c.r8.u32, c.r9.u32, c.r10.u32, c.r11.u32, c.r12.u32, c.r31.u32);
            // Cadeia de frames: [r1] = r1 do chamador; o LR do chamador fica em [r1_do_chamador - 8].
            uint32_t frame = c.r1.u32;
            std::fprintf(stderr, "[Crash] pilha do guest:");
            for (int i = 0; i < 24; i++) {
                const uint32_t previous = GuestLoad(frame);
                if (previous <= frame || previous - frame > 0x100000) break;
                std::fprintf(stderr, " %08X", GuestLoad(previous - 8));
                frame = previous;
            }
            std::fprintf(stderr, "\n");

            // GL_DUMP=<endereco hex>: 128 bytes do guest nesse endereco.
            if (const char* dump = std::getenv("GL_DUMP")) {
                const uint32_t start = static_cast<uint32_t>(std::strtoul(dump, nullptr, 16));
                for (uint32_t line = 0; line < 8; line++) {
                    std::fprintf(stderr, "[Crash] [dump %08X]:", start + line * 16);
                    for (uint32_t i = 0; i < 4; i++) std::fprintf(stderr, " %08X", GuestLoad(start + line * 16 + i * 4));
                    std::fprintf(stderr, "\n");
                }
            }

            // Primeiros 32 bytes apontados por alguns registradores (estruturas do jogo).
            const struct { const char* name; uint32_t value; } probes[] = {
                {"r3", c.r3.u32}, {"r4", c.r4.u32}, {"r29", c.r29.u32}, {"r30", c.r30.u32}, {"r31", c.r31.u32}};
            for (const auto& probe : probes) {
                if (probe.value < 0x10000 || probe.value > 0xFFFFFFE0) continue;
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(g_guestBase + probe.value, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
                std::fprintf(stderr, "[Crash] [%s=%08X]:", probe.name, probe.value);
                for (uint32_t i = 0; i < 8; i++) std::fprintf(stderr, " %08X", GuestLoad(probe.value + i * 4));
                std::fprintf(stderr, "\n");
            }
        }

        void PrintGuestState() { PrintGuestStateFor(t_context); }

        // O codigo gerado emite __imp__sub_X seguido do wrapper sub_X (o que PPCFuncMappings aponta).
        // O corpo de uma funcao fica portanto logo ANTES do wrapper dela: o dono de um rip e o
        // primeiro wrapper com endereco maior.
        const Entry* FindOwner(uintptr_t rip) {
            auto it = std::upper_bound(g_entries.begin(), g_entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            return it == g_entries.end() ? nullptr : &*it;
        }

        const char* ExceptionName(DWORD code) {
            switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_BREAKPOINT: return "BREAKPOINT (trap do guest)";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
            case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
            default: return "EXCECAO";
            }
        }

        // GL_WATCH=<endereco hex do guest>: loga toda escrita naquele endereco (+-8 bytes). A pagina fica
        // somente leitura; cada escrita e executada em passo unico (trap flag) e a protecao e reposta.
        uint32_t g_watchAddress = 0;
        uintptr_t g_watchPage = 0;
        thread_local bool t_stepping = false;
        void PrintGuestState();

        LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
            const DWORD code = info->ExceptionRecord->ExceptionCode;

            if (g_watchPage != 0) {
                if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0] == 1) {
                    const uintptr_t address = info->ExceptionRecord->ExceptionInformation[1];
                    if (address >= g_watchPage && address < g_watchPage + 0x1000) {
                        const uint32_t guest = static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(g_guestBase));
                        if (guest + 8 >= g_watchAddress && guest <= g_watchAddress + 8) {
                            std::fprintf(stderr, "[watch] escrita em %08X rva=0x%llX\n", guest,
                                         static_cast<unsigned long long>(info->ContextRecord->Rip - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
                            PrintGuestState();
                        }
                        DWORD old;
                        VirtualProtect(reinterpret_cast<void*>(g_watchPage), 0x1000, PAGE_READWRITE, &old);
                        info->ContextRecord->EFlags |= 0x100;  // trap flag
                        t_stepping = true;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                if (code == EXCEPTION_SINGLE_STEP && t_stepping) {
                    t_stepping = false;
                    DWORD old;
                    VirtualProtect(reinterpret_cast<void*>(g_watchPage), 0x1000, PAGE_READONLY, &old);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_BREAKPOINT:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_DATATYPE_MISALIGNMENT:
                break;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (InterlockedExchange(&g_reporting, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

            std::fprintf(stderr, "\n[Crash] %s (0x%08lX) em rip=%p\n", ExceptionName(code), code,
                         reinterpret_cast<void*>(info->ContextRecord->Rip));
            if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
                const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
                const uintptr_t address = info->ExceptionRecord->ExceptionInformation[1];
                std::fprintf(stderr, "[Crash] %s do endereco host %p", kind == 0 ? "leitura" : (kind == 1 ? "escrita" : "execucao"),
                             reinterpret_cast<void*>(address));
                if (g_guestBase && address >= reinterpret_cast<uintptr_t>(g_guestBase) &&
                    address < reinterpret_cast<uintptr_t>(g_guestBase) + 0x100000000ull) {
                    std::fprintf(stderr, " = endereco do guest 0x%08X", static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(g_guestBase)));
                }
                std::fprintf(stderr, "\n");
            }

            PrintGuestState();

            // Backtrace do host, com cada quadro mapeado para a funcao do guest provavel.
            CONTEXT context = *info->ContextRecord;
            for (int frame = 0; frame < 48; frame++) {
                const uintptr_t rip = context.Rip;
                const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                std::fprintf(stderr, "[Crash]  #%02d rip=%p rva=0x%llX\n", frame, reinterpret_cast<void*>(rip),
                             static_cast<unsigned long long>(rip - moduleBase));

                DWORD64 imageBase = 0;
                RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
                if (!function) {
                    if (context.Rsp == 0) break;
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                } else {
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, rip, function, &context, &handlerData, &establisher, nullptr);
                }
                if (context.Rip == 0) break;
            }
            std::fflush(stderr);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void SetCrashContext(void* ppcContext) {
        t_context = static_cast<PPCContext*>(ppcContext);
        HANDLE real = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, THREAD_ALL_ACCESS, FALSE, 0);
        std::lock_guard<std::mutex> lock(g_threadsLock);
        g_threads.push_back({real, GetCurrentThreadId(), t_context});
    }

    void InstallCrashHandler(uint8_t* guestBase) {
        g_guestBase = guestBase;
        g_entries.clear();
        for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++) {
            g_entries.push_back({reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), static_cast<uint32_t>(PPCFuncMappings[i].guest)});
        }
        std::sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
        AddVectoredExceptionHandler(1, Handler);

        // GL_SAMPLE=<segundos>: depois desse tempo, e a cada intervalo, mostra onde cada thread do guest esta
        // (rva do host + registradores/pilha do guest). Util para achar threads presas.
        if (const char* sample = std::getenv("GL_SAMPLE")) {
            const int seconds = std::atoi(sample);
            std::thread([seconds] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds > 0 ? seconds : 10));
                    std::lock_guard<std::mutex> lock(g_threadsLock);
                    std::fprintf(stderr, "\n[sample] ---- %zu threads ----\n", g_threads.size());
                    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    for (const SampledThread& t : g_threads) {
                        if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
                        CONTEXT cx{};
                        cx.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(t.handle, &cx)) {
                            std::fprintf(stderr, "[sample] thread host %lu rva=0x%llX\n", t.id,
                                         static_cast<unsigned long long>(cx.Rip - moduleBase));
                            PrintGuestStateFor(t.context);
                            for (int frame = 1; frame < 6; frame++) {
                                DWORD64 imageBase = 0;
                                RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(cx.Rip, &imageBase, nullptr);
                                if (!fn) break;
                                void* handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, cx.Rip, fn, &cx, &handlerData, &establisher, nullptr);
                                if (cx.Rip == 0) break;
                                std::fprintf(stderr, "[sample]   #%02d rva=0x%llX\n", frame, static_cast<unsigned long long>(cx.Rip - moduleBase));
                            }
                        }
                        ResumeThread(t.handle);
                    }
                    std::fflush(stderr);
                }
            }).detach();
        }

        if (const char* watch = std::getenv("GL_WATCH")) {
            g_watchAddress = static_cast<uint32_t>(std::strtoul(watch, nullptr, 16));
            g_watchPage = reinterpret_cast<uintptr_t>(guestBase) + (g_watchAddress & ~0xFFFu);
            std::fprintf(stderr, "[watch] observando %08X\n", g_watchAddress);
            // A pagina so existe depois que o jogo a aloca (e uma nova alocacao repoe a protecao):
            // uma thread a mantem somente leitura.
            std::thread([] {
                for (;;) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<void*>(g_watchPage), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
                        mbi.Protect == PAGE_READWRITE && !t_stepping) {
                        DWORD old;
                        VirtualProtect(reinterpret_cast<void*>(g_watchPage), 0x1000, PAGE_READONLY, &old);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }).detach();
        }
    }
#else
    void InstallCrashHandler(uint8_t*) {}
    void SetCrashContext(void*) {}
#endif

}  // namespace Platform
