// Imports do xboxkrnl.exe para threads, sincronizacao, secoes criticas, TLS, tempo e espera.
// Convencao do codigo recompilado: argumentos em r3..r10 (9o em diante na pilha em r1 + 0x54 + 8n),
// retorno em r3. Ponteiros do guest sao enderecos relativos a `base`, com dados big-endian.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Kernel;

namespace {
    constexpr uint32_t kMaxTlsSlots = 1024;
    std::atomic<uint32_t> g_nextTlsSlot{1};

    uint32_t Ld32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
    int64_t Ld64(uint8_t* base, uint32_t addr) { return static_cast<int64_t>(Endian::Swap64(*reinterpret_cast<uint64_t*>(base + addr))); }
    void St64(uint8_t* base, uint32_t addr, uint64_t value) { *reinterpret_cast<uint64_t*>(base + addr) = Endian::Swap64(value); }

    // Lê o ponteiro de timeout (LARGE_INTEGER*) do guest; nullptr = infinito.
    const int64_t* TimeoutArg(uint8_t* base, uint32_t ptr, int64_t& storage) {
        if (!ptr) return nullptr;
        storage = Ld64(base, ptr);
        return &storage;
    }

    void SleepFor100ns(int64_t interval) {
        if (interval < 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds((-interval) * 100));
        } else if (interval > 0) {
            const int64_t now = static_cast<int64_t>(NowFileTime());
            if (interval > now) std::this_thread::sleep_for(std::chrono::nanoseconds((interval - now) * 100));
        } else {
            std::this_thread::yield();
        }
    }

    std::shared_ptr<KObject> ObjectFromHandleOrPtr(uint8_t* base, uint32_t value) {
        // Handles ficam em 0xF8000000+; qualquer outro valor e tratado como ponteiro de dispatcher.
        if (value >= 0xF8000000u) return LookupHandle(value);
        return LookupGuestObject(base, value);
    }

    uint32_t DoWait(uint8_t* base, std::shared_ptr<KObject> object, uint32_t timeoutPtr) {
        int64_t storage = 0;
        const int64_t* timeout = TimeoutArg(base, timeoutPtr, storage);
        return WaitForObjects({std::move(object)}, false, timeout);
    }
}

// --- Eventos -------------------------------------------------------------------------------

// r3 = handle*, r4 = OBJECT_ATTRIBUTES*, r5 = tipo (0 = notificacao/manual, 1 = sincronizacao/auto), r6 = estado inicial
KERNEL_FUNC(__imp__NtCreateEvent) {
    auto event = std::make_shared<KEvent>(ctx.r5.u32 == 0, ctx.r6.u32 != 0);
    const uint32_t handle = AddHandle(event);
    if (ctx.r3.u32) St32(base, ctx.r3.u32, handle);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = estado anterior*
KERNEL_FUNC(__imp__NtSetEvent) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != KObject::Type::Event) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    const uint32_t previous = SetEvent(static_cast<KEvent&>(*object), base);
    if (ctx.r4.u32) St32(base, ctx.r4.u32, previous);
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__NtClearEvent) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != KObject::Type::Event) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    ResetEvent(static_cast<KEvent&>(*object), base);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = KEVENT*, r4 = incremento, r5 = wait
KERNEL_FUNC(__imp__KeSetEvent) {
    auto object = LookupGuestObject(base, ctx.r3.u32);
    if (!object || object->type != KObject::Type::Event) { ctx.r3.u64 = 0; return; }
    ctx.r3.u64 = SetEvent(static_cast<KEvent&>(*object), base);
}

KERNEL_FUNC(__imp__KeResetEvent) {
    auto object = LookupGuestObject(base, ctx.r3.u32);
    if (!object || object->type != KObject::Type::Event) { ctx.r3.u64 = 0; return; }
    ctx.r3.u64 = ResetEvent(static_cast<KEvent&>(*object), base);
}

// --- Semaforos e mutantes --------------------------------------------------------------------

// r3 = handle*, r4 = OBJECT_ATTRIBUTES*, r5 = contagem inicial, r6 = maximo
KERNEL_FUNC(__imp__NtCreateSemaphore) {
    auto semaphore = std::make_shared<KSemaphore>(static_cast<int32_t>(ctx.r5.u32), static_cast<int32_t>(ctx.r6.u32));
    const uint32_t handle = AddHandle(semaphore);
    if (ctx.r3.u32) St32(base, ctx.r3.u32, handle);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = quantidade a liberar, r5 = contagem anterior*
KERNEL_FUNC(__imp__NtReleaseSemaphore) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != KObject::Type::Semaphore) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    auto& semaphore = static_cast<KSemaphore&>(*object);
    std::lock_guard<std::mutex> lock(Lock());
    const int32_t previous = semaphore.count;
    const int32_t release = static_cast<int32_t>(ctx.r4.u32);
    if (release < 0 || previous + int64_t(release) > semaphore.limit) { ctx.r3.u64 = Status::kInvalidParameter; return; }
    semaphore.count += release;
    semaphore.SyncGuestHeader(base);
    NotifyStateChanged();
    if (ctx.r5.u32) St32(base, ctx.r5.u32, static_cast<uint32_t>(previous));
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle*, r4 = OBJECT_ATTRIBUTES*, r5 = dono inicial
KERNEL_FUNC(__imp__NtCreateMutant) {
    auto mutant = std::make_shared<KMutant>();
    if (ctx.r5.u32) {
        std::lock_guard<std::mutex> lock(Lock());
        mutant->Acquire(CurrentThread().id);
    }
    const uint32_t handle = AddHandle(mutant);
    if (ctx.r3.u32) St32(base, ctx.r3.u32, handle);
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__NtReleaseMutant) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != KObject::Type::Mutant) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    auto& mutant = static_cast<KMutant&>(*object);
    std::lock_guard<std::mutex> lock(Lock());
    if (mutant.owner != CurrentThread().id) { ctx.r3.u64 = Status::kAccessDenied; return; }
    if (--mutant.recursion <= 0) { mutant.recursion = 0; mutant.owner = 0; }
    NotifyStateChanged();
    ctx.r3.u64 = Status::kSuccess;
}

// --- Handles ---------------------------------------------------------------------------------

KERNEL_FUNC(__imp__NtClose) {
    ctx.r3.u64 = RemoveHandle(ctx.r3.u32) ? Status::kSuccess : Status::kInvalidHandle;
}

// r3 = handle, r4 = novo handle*, r5 = opcoes (1 = fechar a origem)
KERNEL_FUNC(__imp__NtDuplicateObject) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    const uint32_t duplicate = AddHandle(object);
    if (ctx.r4.u32) St32(base, ctx.r4.u32, duplicate);
    if (ctx.r5.u32 & 1) RemoveHandle(ctx.r3.u32);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = tipo de objeto*, r5 = modo, r6 = objeto*  -> o "objeto" e um ponteiro de dispatcher no guest
KERNEL_FUNC(__imp__ObReferenceObjectByHandle) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    const uint32_t ptr = GuestPointerOf(base, object);
    if (!ptr) { ctx.r3.u64 = Status::kNoMemory; return; }
    if (ctx.r6.u32) St32(base, ctx.r6.u32, ptr);
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__ObReferenceObject) { (void)ctx; (void)base; }
KERNEL_FUNC(__imp__ObDereferenceObject) { (void)ctx; (void)base; }
KERNEL_FUNC(__imp__ObIsTitleObject) { ctx.r3.u64 = 0; }

// --- Espera -----------------------------------------------------------------------------------

// r3 = handle, r4 = modo, r5 = alertable, r6 = timeout*
KERNEL_FUNC(__imp__NtWaitForSingleObjectEx) {
    auto object = ObjectFromHandleOrPtr(base, ctx.r3.u32);
    ctx.r3.u64 = object ? DoWait(base, std::move(object), ctx.r6.u32) : Status::kInvalidHandle;
}

// r3 = objeto*, r4 = motivo, r5 = modo, r6 = alertable, r7 = timeout*
KERNEL_FUNC(__imp__KeWaitForSingleObject) {
    auto object = ObjectFromHandleOrPtr(base, ctx.r3.u32);
    ctx.r3.u64 = object ? DoWait(base, std::move(object), ctx.r7.u32) : Status::kInvalidHandle;
}

// r3 = quantidade, r4 = handles*, r5 = tipo (0 = todos, 1 = qualquer), r6 = modo, r7 = alertable, r8 = timeout*
KERNEL_FUNC(__imp__NtWaitForMultipleObjectsEx) {
    const uint32_t count = ctx.r3.u32;
    std::vector<std::shared_ptr<KObject>> objects;
    for (uint32_t i = 0; i < count && i < 64; i++) objects.push_back(LookupHandle(Ld32(base, ctx.r4.u32 + i * 4)));
    int64_t storage = 0;
    ctx.r3.u64 = WaitForObjects(objects, ctx.r5.u32 == 0, TimeoutArg(base, ctx.r8.u32, storage));
}

// r3 = quantidade, r4 = objetos*, r5 = tipo, r6 = motivo, r7 = modo, r8 = alertable, r9 = timeout*, r10 = wait blocks*
KERNEL_FUNC(__imp__KeWaitForMultipleObjects) {
    const uint32_t count = ctx.r3.u32;
    std::vector<std::shared_ptr<KObject>> objects;
    for (uint32_t i = 0; i < count && i < 64; i++) objects.push_back(ObjectFromHandleOrPtr(base, Ld32(base, ctx.r4.u32 + i * 4)));
    if (g_traceKernel) {
        for (uint32_t i = 0; i < count && i < 64; i++) {
            if (!objects[i]) std::fprintf(stderr, "[trace]   KeWaitForMultipleObjects: objeto %u invalido (valor %08X)\n", i, Ld32(base, ctx.r4.u32 + i * 4));
        }
    }
    int64_t storage = 0;
    ctx.r3.u64 = WaitForObjects(objects, ctx.r5.u32 == 0, TimeoutArg(base, ctx.r9.u32, storage));
}

// r3 = modo, r4 = alertable, r5 = intervalo*
KERNEL_FUNC(__imp__KeDelayExecutionThread) {
    int64_t storage = 0;
    const int64_t* interval = TimeoutArg(base, ctx.r5.u32, storage);
    // GL_TRACE_TIMEOUTS: tambem agrega os sleeps (thread, prazo, chamador) com resumo a cada 10 s.
    static const bool traceSleeps = std::getenv("GL_TRACE_TIMEOUTS") != nullptr;
    if (traceSleeps && interval) {
        static std::mutex lock;
        static std::map<std::string, uint32_t> counts;
        static auto lastReport = std::chrono::steady_clock::now();
        char key[80];
        std::snprintf(key, sizeof(key), "t%u sleep=%lldms lr=%08X", CurrentThread().id,
                      static_cast<long long>(*interval < 0 ? -*interval / 10000 : -1), static_cast<uint32_t>(ctx.lr));
        std::lock_guard<std::mutex> guard(lock);
        ++counts[key];
        if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(10)) {
            lastReport = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[sleeps] ----\n");
            for (const auto& [k, v] : counts) {
                if (v >= 3) std::fprintf(stderr, "[sleeps] %6u x %s\n", v, k.c_str());
            }
            counts.clear();
        }
    }
    if (interval) SleepFor100ns(*interval);
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__NtYieldExecution) {
    std::this_thread::yield();
    ctx.r3.u64 = Status::kSuccess;
}

// --- Threads ---------------------------------------------------------------------------------

// r3 = handle*, r4 = tamanho da stack, r5 = id*, r6 = xapi_thread_startup, r7 = start_address,
// r8 = start_context, r9 = flags (bit 0 = criar suspensa)
KERNEL_FUNC(__imp__ExCreateThread) {
    ThreadStart start;
    start.entry = ctx.r7.u32;
    start.startupThunk = ctx.r6.u32;
    start.argument = ctx.r8.u32;
    start.stackSize = ctx.r4.u32 ? ctx.r4.u32 : 0x40000;
    start.suspended = (ctx.r9.u32 & 1) != 0;

    auto thread = StartGuestThread(base, start);
    if (!thread) { ctx.r3.u64 = Status::kNoMemory; return; }
    const uint32_t handle = AddHandle(thread);
    if (ctx.r3.u32) St32(base, ctx.r3.u32, handle);
    if (ctx.r5.u32) St32(base, ctx.r5.u32, thread->id);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = contagem de suspensao anterior*
KERNEL_FUNC(__imp__NtResumeThread) {
    auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != KObject::Type::Thread) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    const uint32_t previous = ResumeThread(static_cast<KThread&>(*object));
    if (ctx.r4.u32) St32(base, ctx.r4.u32, previous);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = codigo de saida: desenrola a thread host ate o inicio da thread
KERNEL_FUNC(__imp__ExTerminateThread) {
    (void)base;
    throw ThreadExit{ctx.r3.u32};
}

KERNEL_FUNC(__imp__KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
KERNEL_FUNC(__imp__KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }
KERNEL_FUNC(__imp__KeSetAffinityThread) { ctx.r3.u64 = 1; }
KERNEL_FUNC(__imp__KeGetCurrentProcessType) { ctx.r3.u64 = 1; }  // usuario
KERNEL_FUNC(__imp__KeSetCurrentProcessType) { (void)ctx; }
KERNEL_FUNC(__imp__KeLockL2) { (void)ctx; }
KERNEL_FUNC(__imp__KeUnlockL2) { (void)ctx; }
KERNEL_FUNC(__imp__KeEnterCriticalRegion) { (void)ctx; }
KERNEL_FUNC(__imp__KeLeaveCriticalRegion) { (void)ctx; }

// APCs e DPCs: o port nao as entrega; o jogo as usa para IO assincrono do XAM.
KERNEL_FUNC(__imp__NtQueueApcThread) { ctx.r3.u64 = Status::kSuccess; }
KERNEL_FUNC(__imp__KeInitializeApc) { (void)ctx; }
KERNEL_FUNC(__imp__KeInsertQueueApc) { ctx.r3.u64 = 0; }
KERNEL_FUNC(__imp__KeInitializeDpc) { (void)ctx; }
KERNEL_FUNC(__imp__KeInsertQueueDpc) { ctx.r3.u64 = 0; }
KERNEL_FUNC(__imp__KiApcNormalRoutineNop) { (void)ctx; }

// Spinlocks: o IRQL nao e emulado; o lock em si e um spin sobre o proprio dword do guest.
KERNEL_FUNC(__imp__KfAcquireSpinLock) {
    auto* lock = reinterpret_cast<std::atomic<uint32_t>*>(base + ctx.r3.u32);
    const uint32_t me = CurrentThread().id;
    uint32_t expected = 0;
    while (!lock->compare_exchange_weak(expected, me)) {
        expected = 0;
        std::this_thread::yield();
    }
    ctx.r3.u64 = 0;  // IRQL anterior
}

KERNEL_FUNC(__imp__KfReleaseSpinLock) {
    reinterpret_cast<std::atomic<uint32_t>*>(base + ctx.r3.u32)->store(0);
}

KERNEL_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) { __imp__KfAcquireSpinLock(ctx, base); }
KERNEL_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { __imp__KfReleaseSpinLock(ctx, base); }

// --- TLS -------------------------------------------------------------------------------------

KERNEL_FUNC(__imp__KeTlsAlloc) {
    const uint32_t slot = g_nextTlsSlot.fetch_add(1);
    ctx.r3.u64 = slot < kMaxTlsSlots ? slot : 0xFFFFFFFFu;
}

KERNEL_FUNC(__imp__KeTlsFree) { ctx.r3.u64 = 1; }

KERNEL_FUNC(__imp__KeTlsGetValue) {
    ThreadInfo& info = CurrentThread();
    auto it = info.tls.find(ctx.r3.u32);
    ctx.r3.u64 = it == info.tls.end() ? 0 : it->second;
}

KERNEL_FUNC(__imp__KeTlsSetValue) {
    CurrentThread().tls[ctx.r3.u32] = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// --- Secoes criticas -------------------------------------------------------------------------
// X_RTL_CRITICAL_SECTION: header (0x10) | lock_count (+0x10) | recursion (+0x14) | dono (+0x18)

namespace {
    constexpr uint32_t kCsLockCount = 0x10;
    constexpr uint32_t kCsRecursion = 0x14;
    constexpr uint32_t kCsOwner = 0x18;

    void InitCriticalSection(uint8_t* base, uint32_t cs) {
        std::memset(base + cs, 0, 0x1C);
        St32(base, cs + 0x00, 1);  // type: sincronizacao
        St32(base, cs + kCsLockCount, 0xFFFFFFFFu);
    }
}

KERNEL_FUNC(__imp__RtlInitializeCriticalSection) { InitCriticalSection(base, ctx.r3.u32); ctx.r3.u64 = 0; }
KERNEL_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) { InitCriticalSection(base, ctx.r3.u32); ctx.r3.u64 = 0; }

KERNEL_FUNC(__imp__RtlEnterCriticalSection) {
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = CurrentThread().id;
    std::unique_lock<std::mutex> lock(Lock());
    for (;;) {
        const uint32_t owner = Ld32(base, cs + kCsOwner);
        if (owner == 0 || owner == me) {
            St32(base, cs + kCsOwner, me);
            St32(base, cs + kCsRecursion, Ld32(base, cs + kCsRecursion) + 1);
            St32(base, cs + kCsLockCount, Ld32(base, cs + kCsLockCount) + 1);
            break;
        }
        Cv().wait(lock);
    }
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(__imp__RtlTryEnterCriticalSection) {
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = CurrentThread().id;
    std::lock_guard<std::mutex> lock(Lock());
    const uint32_t owner = Ld32(base, cs + kCsOwner);
    if (owner == 0 || owner == me) {
        St32(base, cs + kCsOwner, me);
        St32(base, cs + kCsRecursion, Ld32(base, cs + kCsRecursion) + 1);
        St32(base, cs + kCsLockCount, Ld32(base, cs + kCsLockCount) + 1);
        ctx.r3.u64 = 1;
    } else {
        ctx.r3.u64 = 0;
    }
}

KERNEL_FUNC(__imp__RtlLeaveCriticalSection) {
    const uint32_t cs = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(Lock());
    const uint32_t recursion = Ld32(base, cs + kCsRecursion);
    if (recursion > 0) {
        St32(base, cs + kCsRecursion, recursion - 1);
        St32(base, cs + kCsLockCount, Ld32(base, cs + kCsLockCount) - 1);
        if (recursion == 1) {
            St32(base, cs + kCsOwner, 0);
            NotifyStateChanged();
        }
    }
    ctx.r3.u64 = 0;
}

// --- Tempo -----------------------------------------------------------------------------------

KERNEL_FUNC(__imp__KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }

// r3 = FILETIME*
KERNEL_FUNC(__imp__KeQuerySystemTime) {
    if (ctx.r3.u32) St64(base, ctx.r3.u32, NowFileTime());
}

namespace {
    // TIME_FIELDS (8 x u16 big-endian): ano, mes, dia, hora, minuto, segundo, ms, dia da semana.
    constexpr uint64_t kTicksPerMs = 10000;
    constexpr uint64_t kTicksPerSecond = 10000000;
    constexpr uint64_t kTicksPerDay = 86400ull * kTicksPerSecond;

    bool IsLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    int DaysInMonth(int64_t y, int m) {
        static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return m == 1 && IsLeap(y) ? 29 : days[m];
    }
}

// r3 = FILETIME*, r4 = TIME_FIELDS*
KERNEL_FUNC(__imp__RtlTimeToTimeFields) {
    const uint64_t time = static_cast<uint64_t>(Ld64(base, ctx.r3.u32));
    uint64_t days = time / kTicksPerDay;
    const uint64_t rest = time % kTicksPerDay;
    const uint32_t weekday = static_cast<uint32_t>((days + 1) % 7);  // 1601-01-01 foi uma segunda-feira

    int64_t year = 1601;
    for (;;) {
        const uint64_t length = IsLeap(year) ? 366 : 365;
        if (days < length) break;
        days -= length;
        ++year;
    }
    int month = 0;
    while (days >= static_cast<uint64_t>(DaysInMonth(year, month))) {
        days -= DaysInMonth(year, month);
        ++month;
    }

    const uint32_t fields = ctx.r4.u32;
    auto put = [&](uint32_t index, uint32_t value) {
        base[fields + index * 2] = static_cast<uint8_t>(value >> 8);
        base[fields + index * 2 + 1] = static_cast<uint8_t>(value);
    };
    put(0, static_cast<uint32_t>(year));
    put(1, month + 1);
    put(2, static_cast<uint32_t>(days) + 1);
    put(3, static_cast<uint32_t>(rest / (3600 * kTicksPerSecond)));
    put(4, static_cast<uint32_t>((rest / (60 * kTicksPerSecond)) % 60));
    put(5, static_cast<uint32_t>((rest / kTicksPerSecond) % 60));
    put(6, static_cast<uint32_t>((rest / kTicksPerMs) % 1000));
    put(7, weekday);
}

// r3 = TIME_FIELDS*, r4 = FILETIME* -> 1 em sucesso
KERNEL_FUNC(__imp__RtlTimeFieldsToTime) {
    auto get = [&](uint32_t index) -> uint32_t {
        return (uint32_t(base[ctx.r3.u32 + index * 2]) << 8) | base[ctx.r3.u32 + index * 2 + 1];
    };
    const int64_t year = get(0);
    const int month = static_cast<int>(get(1));
    if (year < 1601 || month < 1 || month > 12) { ctx.r3.u64 = 0; return; }

    uint64_t days = 0;
    for (int64_t y = 1601; y < year; y++) days += IsLeap(y) ? 366 : 365;
    for (int m = 0; m < month - 1; m++) days += DaysInMonth(year, m);
    days += get(2) - 1;

    const uint64_t ticks = days * kTicksPerDay + get(3) * 3600ull * kTicksPerSecond +
                           get(4) * 60ull * kTicksPerSecond + get(5) * kTicksPerSecond + get(6) * kTicksPerMs;
    St64(base, ctx.r4.u32, ticks);
    ctx.r3.u64 = 1;
}
