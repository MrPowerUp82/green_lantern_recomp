#include "kernel_state.h"

#include "memory.h"
#include "platform/crash_handler.h"
#include "recompiled/ppc_recomp_shared.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Kernel {

    namespace {
        std::mutex g_lock;
        std::condition_variable g_cv;

        std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
        std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_guestObjects;
        uint32_t g_nextHandle = 0xF8000004;
        std::atomic<uint32_t> g_nextThreadId{2};  // 1 = thread principal

        // Tipos do DISPATCHER_HEADER do kernel do Xbox 360.
        constexpr uint8_t kTypeNotificationEvent = 0;
        constexpr uint8_t kTypeSynchronizationEvent = 1;
        constexpr uint8_t kTypeMutant = 2;
        constexpr uint8_t kTypeSemaphore = 5;
        constexpr uint8_t kTypeThread = 6;

        void StoreBe32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
        uint32_t LoadBe32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    }

    bool g_traceKernel = [] {
        const char* value = std::getenv("GL_TRACE_KERNEL");
        return value && *value && *value != '0';
    }();

    void TraceCall(const char* name, const PPCContext& ctx, bool entry) {
        const uint32_t tid = CurrentThread().id;
        if (entry) {
            std::fprintf(stderr, "[trace t%u] %s(%08X, %08X, %08X, %08X, %08X, %08X, %08X) lr=%08X\n", tid,
                         name + (std::strncmp(name, "__imp__", 7) == 0 ? 7 : 0),
                         ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, static_cast<uint32_t>(ctx.lr));
        } else {
            std::fprintf(stderr, "[trace t%u]   -> %08X\n", tid, ctx.r3.u32);
        }
    }

    std::mutex& Lock() { return g_lock; }
    std::condition_variable& Cv() { return g_cv; }

    void NotifyStateChanged() { g_cv.notify_all(); }

    // --- Dispatcher headers -----------------------------------------------------------------
    // Como o Xenia: o wait_list (+8/+C) de um header que o HLE conhece recebe uma assinatura. O guest
    // reinicializa eventos inline (KeInitializeEvent do XDK escreve o header direto, com wait_list
    // apontando para si mesmo); se a assinatura sumiu, o objeto em cache e de uma vida anterior daquela
    // memoria e e recriado a partir do header atual. Sem isso, um evento sinalizado de um objeto ja
    // destruido "vazava" para o novo objeto alocado no mesmo endereco.
    namespace {
        constexpr uint32_t kHeaderSignature = 0x58454E20;  // 'XEN '

        void StampHeader(uint8_t* base, uint32_t ptr) {
            StoreBe32(base, ptr + 8, kHeaderSignature);
            StoreBe32(base, ptr + 12, kHeaderSignature);
        }

        bool HeaderStamped(uint8_t* base, uint32_t ptr) {
            return LoadBe32(base, ptr + 8) == kHeaderSignature && LoadBe32(base, ptr + 12) == kHeaderSignature;
        }
    }

    void KEvent::SyncGuestHeader(uint8_t* base) {
        if (!guestPtr || !base) return;
        base[guestPtr] = manual ? kTypeNotificationEvent : kTypeSynchronizationEvent;
        StoreBe32(base, guestPtr + 4, signaled ? 1 : 0);
        StampHeader(base, guestPtr);
    }

    void KSemaphore::SyncGuestHeader(uint8_t* base) {
        if (!guestPtr || !base) return;
        base[guestPtr] = kTypeSemaphore;
        StoreBe32(base, guestPtr + 4, static_cast<uint32_t>(count));
        StoreBe32(base, guestPtr + 0x10, static_cast<uint32_t>(limit));
        StampHeader(base, guestPtr);
    }

    void KThread::SyncGuestHeader(uint8_t* base) {
        if (!guestPtr || !base) return;
        base[guestPtr] = kTypeThread;
        StoreBe32(base, guestPtr + 4, terminated ? 1 : 0);
        StampHeader(base, guestPtr);
    }

    // --- Handles ----------------------------------------------------------------------------

    uint32_t AddHandle(std::shared_ptr<KObject> object) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        if (g_traceKernel) {
            static const char* kTypes[] = {"evento", "semaforo", "mutante", "thread", "arquivo", "outro"};
            std::fprintf(stderr, "[trace]   handle %08X = %s\n", handle, kTypes[static_cast<int>(object->type)]);
        }
        g_handles[handle] = std::move(object);
        return handle;
    }

    std::shared_ptr<KObject> LookupHandle(uint32_t handle) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_handles.find(handle);
        return it == g_handles.end() ? nullptr : it->second;
    }

    bool RemoveHandle(uint32_t handle) {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_handles.erase(handle) != 0;
    }

    std::shared_ptr<KObject> LookupGuestObject(uint8_t* base, uint32_t ptr) {
        if (ptr < 0x10000 || !base) return nullptr;  // ponteiro invalido (pagina nula)
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_guestObjects.find(ptr);
        if (it != g_guestObjects.end()) {
            // Header ainda e o nosso: mesmo objeto. Threads (KTHREAD do HLE) e mutantes nao sao recriados.
            const KObject::Type cachedType = it->second->type;
            if (HeaderStamped(base, ptr) || cachedType == KObject::Type::Thread || cachedType == KObject::Type::Mutant) {
                return it->second;
            }
            // O guest reinicializou essa memoria: descarta o objeto antigo (quem ja o tem continua valido).
            g_guestObjects.erase(it);
        }

        std::shared_ptr<KObject> object;
        const uint8_t type = base[ptr];
        const uint32_t state = LoadBe32(base, ptr + 4);
        switch (type) {
        case kTypeNotificationEvent:
            object = std::make_shared<KEvent>(true, state != 0);
            break;
        case kTypeSynchronizationEvent:
            object = std::make_shared<KEvent>(false, state != 0);
            break;
        case kTypeSemaphore:
            object = std::make_shared<KSemaphore>(static_cast<int32_t>(state), static_cast<int32_t>(LoadBe32(base, ptr + 0x10)));
            break;
        case kTypeMutant:
            object = std::make_shared<KMutant>();
            break;
        default:
            std::cerr << "[Kernel] objeto de dispatcher desconhecido em 0x" << std::hex << ptr << " tipo " << int(type)
                      << std::dec << std::endl;
            return nullptr;
        }
        object->guestPtr = ptr;
        g_guestObjects[ptr] = object;
        StampHeader(base, ptr);
        return object;
    }

    uint32_t GuestPointerOf(uint8_t* base, const std::shared_ptr<KObject>& object) {
        if (!object) return 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (object->guestPtr) return object->guestPtr;
        }
        const GuestAddr ptr = MemoryManager::AllocateAligned(0x40, 0x10, false);
        if (!ptr) return 0;
        std::lock_guard<std::mutex> lock(g_lock);
        if (object->guestPtr) return object->guestPtr;  // outra thread foi mais rapida (vaza 4 KB, raro)
        object->guestPtr = ptr;
        g_guestObjects[ptr] = object;
        object->SyncGuestHeader(base);
        return ptr;
    }

    // --- Espera -----------------------------------------------------------------------------

    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objects, bool waitAll, const int64_t* timeout100ns) {
        for (const auto& object : objects) {
            if (!object) return Status::kInvalidHandle;
        }
        const uint32_t tid = CurrentThread().id;
        uint8_t* base = MemoryManager::GetBase();

        using Clock = std::chrono::steady_clock;
        bool infinite = timeout100ns == nullptr;
        Clock::time_point deadline{};
        if (!infinite) {
            int64_t relative100ns;
            const int64_t value = *timeout100ns;
            if (value < 0) {
                relative100ns = -value;
            } else if (value == 0) {
                relative100ns = 0;
            } else {
                const int64_t now = static_cast<int64_t>(NowFileTime());
                relative100ns = value > now ? value - now : 0;
            }
            deadline = Clock::now() + std::chrono::nanoseconds(relative100ns * 100);
        }

        std::unique_lock<std::mutex> lock(g_lock);
        for (;;) {
            if (waitAll) {
                bool all = true;
                for (const auto& object : objects) {
                    if (!object->Signaled(tid)) { all = false; break; }
                }
                if (all) {
                    for (const auto& object : objects) { object->Acquire(tid); object->SyncGuestHeader(base); }
                    g_cv.notify_all();
                    return 0;
                }
            } else {
                for (size_t i = 0; i < objects.size(); i++) {
                    if (objects[i]->Signaled(tid)) {
                        objects[i]->Acquire(tid);
                        objects[i]->SyncGuestHeader(base);
                        g_cv.notify_all();
                        return static_cast<uint32_t>(i);
                    }
                }
            }

            if (infinite) {
                g_cv.wait(lock);
            } else if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                // Uma ultima verificacao ja foi feita acima no inicio do laco; sai por timeout.
                bool ready = false;
                if (waitAll) {
                    ready = true;
                    for (const auto& object : objects) if (!object->Signaled(tid)) { ready = false; break; }
                } else {
                    for (const auto& object : objects) if (object->Signaled(tid)) { ready = true; break; }
                }
                if (!ready) {
                    // GL_TRACE_TIMEOUTS: agrega esperas que expiram (thread, prazo, tipo e header do 1o
                    // objeto) e imprime um resumo a cada 10 s. Mostra quem avanca so por timeout.
                    static const bool traceTimeouts = std::getenv("GL_TRACE_TIMEOUTS") != nullptr;
                    if (traceTimeouts) {
                        static std::map<std::string, uint32_t> counts;
                        static Clock::time_point lastReport = Clock::now();
                        char key[96];
                        const int64_t ms = *timeout100ns < 0 ? -*timeout100ns / 10000 : *timeout100ns == 0 ? 0 : -1;
                        std::snprintf(key, sizeof(key), "t%u prazo=%lldms tipo=%d obj=%08X n=%zu", tid, static_cast<long long>(ms),
                                      static_cast<int>(objects[0]->type), objects[0]->guestPtr, objects.size());
                        ++counts[key];
                        if (Clock::now() - lastReport > std::chrono::seconds(10)) {
                            lastReport = Clock::now();
                            std::fprintf(stderr, "[timeouts] ----\n");
                            for (const auto& [k, v] : counts) {
                                if (v >= 3) std::fprintf(stderr, "[timeouts] %6u x %s\n", v, k.c_str());
                            }
                            counts.clear();
                        }
                    }
                    return Status::kTimeout;
                }
            }
        }
    }

    uint32_t SetEvent(KEvent& event, uint8_t* base) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uint32_t previous = event.signaled ? 1 : 0;
        event.signaled = true;
        event.SyncGuestHeader(base);
        g_cv.notify_all();
        return previous;
    }

    uint32_t ResetEvent(KEvent& event, uint8_t* base) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uint32_t previous = event.signaled ? 1 : 0;
        event.signaled = false;
        event.SyncGuestHeader(base);
        return previous;
    }

    // --- Threads ----------------------------------------------------------------------------

    ThreadInfo& CurrentThread() {
        thread_local ThreadInfo info;
        if (info.id == 0) info.id = g_nextThreadId.fetch_add(1);
        return info;
    }

    void InitThreadContext(ThreadInfo& info, PPCContext& ctx, GuestSize stackSize) {
        uint8_t* base = MemoryManager::GetBase();

        if (info.pcr == 0) {
            info.pcr = MemoryManager::AllocateAligned(0x1000, 0x1000, false);
            info.kthread = MemoryManager::AllocateAligned(0x1000, 0x1000, false);
            const GuestAddr tlsBlock = MemoryManager::AllocateAligned(0x4000, 0x1000, false);
            StoreBe32(base, info.pcr + 0x00, tlsBlock);
            StoreBe32(base, info.pcr + 0x30, info.pcr);
            StoreBe32(base, info.pcr + 0x100, info.kthread);
            base[info.pcr + 0x10C] = static_cast<uint8_t>(info.id % 6);  // numero do processador
            StoreBe32(base, info.kthread + 0x14C, info.id);  // X_KTHREAD::thread_id
        }

        if (stackSize != 0 && info.stackBase == 0) {
            if (stackSize < 0x10000) stackSize = 0x10000;
            info.stackSize = (stackSize + 0xFFF) & ~0xFFFu;
            info.stackBase = MemoryManager::AllocateAligned(info.stackSize, 0x1000, false);
            ctx.r1.u64 = info.stackBase + info.stackSize - 0x100;
        }
        if (info.stackBase != 0) {
            StoreBe32(base, info.pcr + 0x70, info.stackBase + info.stackSize);
            StoreBe32(base, info.pcr + 0x74, info.stackBase);
        }
        ctx.r13.u64 = info.pcr;
    }

    namespace {
        struct StartPayload {
            ThreadStart start;
            uint8_t* base;
            std::shared_ptr<KThread> thread;
        };

#if defined(_WIN32)
        DWORD WINAPI GuestThreadProc(LPVOID param) {
            std::unique_ptr<StartPayload> payload(static_cast<StartPayload*>(param));
            ThreadInfo& info = CurrentThread();
            info.id = payload->thread->id;
            info.object = payload->thread;

            {
                std::unique_lock<std::mutex> lock(g_lock);
                g_cv.wait(lock, [&] { return !payload->thread->suspended; });
            }

            uint8_t* base = payload->base;
            PPCContext ctx{};
            ctx.fpscr.loadFromHost();
            InitThreadContext(info, ctx, payload->start.stackSize);
            Platform::SetCrashContext(&ctx);
            if (std::getenv("GL_SAMPLE")) {
                std::fprintf(stderr, "[thread] id=%u (0x%X) host=%lu entrada=%08X\n", info.id, info.id, GetCurrentThreadId(),
                             payload->start.entry);
            }

            GuestAddr fn = payload->start.entry;
            if (payload->start.startupThunk != 0) {
                fn = payload->start.startupThunk;
                ctx.r3.u64 = payload->start.entry;
                ctx.r4.u64 = payload->start.argument;
            } else {
                ctx.r3.u64 = payload->start.argument;
            }

            uint32_t exitCode = 0;
            if (g_traceKernel) std::fprintf(stderr, "[trace t%u] thread inicia entrada=%08X thunk=%08X arg=%08X\n", info.id,
                                            payload->start.entry, payload->start.startupThunk, payload->start.argument);
            PPCFunc* host = (fn >= PPC_CODE_BASE && fn < PPC_CODE_BASE + PPC_CODE_SIZE) ? PPC_LOOKUP_FUNC(base, fn) : nullptr;
            if (!host) {
                std::cerr << "[Kernel] thread com entrada invalida: 0x" << std::hex << fn << std::dec << std::endl;
            } else {
                try {
                    host(ctx, base);
                    exitCode = ctx.r3.u32;
                } catch (const ThreadExit& exit) {
                    exitCode = exit.code;
                }
            }

            if (g_traceKernel) std::fprintf(stderr, "[trace t%u] thread terminou codigo=%08X\n", info.id, exitCode);
            {
                std::lock_guard<std::mutex> lock(g_lock);
                payload->thread->terminated = true;
                payload->thread->exitCode = exitCode;
                payload->thread->SyncGuestHeader(base);
                g_cv.notify_all();
            }
            return 0;
        }
#endif
    }

    std::shared_ptr<KThread> StartGuestThread(uint8_t* base, const ThreadStart& start) {
#if defined(_WIN32)
        auto thread = std::make_shared<KThread>();
        thread->id = g_nextThreadId.fetch_add(1);
        thread->suspended = start.suspended;

        auto* payload = new StartPayload{start, base, thread};
        // O codigo recompilado encadeia chamadas nativas: stack host grande, so reservada.
        HANDLE host = CreateThread(nullptr, 16u * 1024 * 1024, GuestThreadProc, payload,
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!host) {
            delete payload;
            return nullptr;
        }
        CloseHandle(host);  // a thread segue viva; o termino e observado por KThread::terminated
        return thread;
#else
        (void)base; (void)start;
        return nullptr;
#endif
    }

    uint32_t ResumeThread(KThread& thread) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uint32_t previous = thread.suspended ? 1 : 0;
        thread.suspended = false;
        g_cv.notify_all();
        return previous;
    }

    // --- Tempo ------------------------------------------------------------------------------

    uint64_t NowFileTime() {
#if defined(_WIN32)
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
        return 0;
#endif
    }

    uint64_t TimebaseTicks() {
#if defined(_WIN32)
        static const uint64_t frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart);
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const uint64_t c = static_cast<uint64_t>(counter.QuadPart);
        constexpr uint64_t kGuestFrequency = 50000000;
        return (c / frequency) * kGuestFrequency + (c % frequency) * kGuestFrequency / frequency;
#else
        return 0;
#endif
    }

}  // namespace Kernel

// Usado pelo codigo recompilado (mftb): relogio de 50 MHz como o do Xbox 360.
extern "C" uint64_t ppc_get_timebase() { return Kernel::TimebaseTicks(); }
