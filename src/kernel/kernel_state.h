#pragma once

// Estado do kernel HLE: tabela de handles, objetos de sincronizacao (evento, semaforo, mutante,
// thread, arquivo), esperas e informacoes por thread (PCR, stack, TLS).
//
// Todos os objetos compartilham um unico mutex e uma unica condition variable: o jogo tem poucas
// threads e isto mantem a semantica de espera (any/all, auto-reset) simples e correta.

#include "types.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PPCContext;

namespace Kernel {
    // Log de chamadas ao kernel (variavel de ambiente GL_TRACE_KERNEL=1): nome, r3..r6 e retorno.
    extern bool g_traceKernel;
    void TraceCall(const char* name, const PPCContext& ctx, bool entry);
}

// Define um import do kernel com trace opcional. Equivale a PPC_FUNC(name) { ... }.
#define KERNEL_FUNC(name)                                                          \
    static void name##_body(PPCContext& ctx, uint8_t* base);                       \
    PPC_FUNC(name) {                                                               \
        if (Kernel::g_traceKernel) Kernel::TraceCall(#name, ctx, true);            \
        name##_body(ctx, base);                                                    \
        if (Kernel::g_traceKernel) Kernel::TraceCall(#name, ctx, false);           \
    }                                                                              \
    static void name##_body(PPCContext& ctx, uint8_t* base)

namespace Kernel {

    namespace Status {
        constexpr uint32_t kSuccess = 0x00000000;
        constexpr uint32_t kUserApc = 0x000000C0;
        constexpr uint32_t kTimeout = 0x00000102;
        constexpr uint32_t kPending = 0x00000103;
        constexpr uint32_t kBufferOverflow = 0x80000005;
        constexpr uint32_t kNoMoreFiles = 0x80000006;
        constexpr uint32_t kUnsuccessful = 0xC0000001;
        constexpr uint32_t kNotImplemented = 0xC0000002;
        constexpr uint32_t kInvalidHandle = 0xC0000008;
        constexpr uint32_t kInvalidParameter = 0xC000000D;
        constexpr uint32_t kNoSuchFile = 0xC000000F;
        constexpr uint32_t kInvalidDeviceRequest = 0xC0000010;
        constexpr uint32_t kEndOfFile = 0xC0000011;
        constexpr uint32_t kNoMemory = 0xC0000017;
        constexpr uint32_t kAccessDenied = 0xC0000022;
        constexpr uint32_t kBufferTooSmall = 0xC0000023;
        constexpr uint32_t kObjectNameNotFound = 0xC0000034;
        constexpr uint32_t kObjectNameCollision = 0xC0000035;
        constexpr uint32_t kObjectPathNotFound = 0xC000003A;
        constexpr uint32_t kNotADirectory = 0xC0000103;
        constexpr uint32_t kFileIsADirectory = 0xC00000BA;
    }

    class KObject {
    public:
        enum class Type { Event, Semaphore, Mutant, Thread, File, Other };

        explicit KObject(Type t) : type(t) {}
        virtual ~KObject() = default;

        // Os tres metodos abaixo sao chamados com o lock global retido.
        virtual bool Signaled(uint32_t threadId) const = 0;
        virtual void Acquire(uint32_t /*threadId*/) {}
        // Espelha o estado no dispatcher header do guest (se houver) para codigo que o le inline.
        virtual void SyncGuestHeader(uint8_t* /*base*/) {}

        Type type;
        uint32_t guestPtr = 0;  // dispatcher header no guest (0 = sem)
    };

    class KEvent : public KObject {
    public:
        KEvent(bool manualReset, bool initial) : KObject(Type::Event), manual(manualReset), signaled(initial) {}
        bool Signaled(uint32_t) const override { return signaled; }
        void Acquire(uint32_t) override { if (!manual) signaled = false; }
        void SyncGuestHeader(uint8_t* base) override;
        bool manual;
        bool signaled;
    };

    class KSemaphore : public KObject {
    public:
        KSemaphore(int32_t initial, int32_t maximum) : KObject(Type::Semaphore), count(initial), limit(maximum) {}
        bool Signaled(uint32_t) const override { return count > 0; }
        void Acquire(uint32_t) override { --count; }
        void SyncGuestHeader(uint8_t* base) override;
        int32_t count;
        int32_t limit;
    };

    class KMutant : public KObject {
    public:
        KMutant() : KObject(Type::Mutant) {}
        bool Signaled(uint32_t tid) const override { return owner == 0 || owner == tid; }
        void Acquire(uint32_t tid) override { owner = tid; ++recursion; }
        uint32_t owner = 0;
        int32_t recursion = 0;
    };

    class KThread : public KObject {
    public:
        KThread() : KObject(Type::Thread) {}
        bool Signaled(uint32_t) const override { return terminated; }
        void SyncGuestHeader(uint8_t* base) override;
        uint32_t id = 0;
        bool terminated = false;
        bool suspended = false;
        uint32_t exitCode = 0;
        int32_t priority = 0;
    };

    // Estado por thread host que executa codigo do guest.
    struct ThreadInfo {
        uint32_t id = 0;
        GuestAddr pcr = 0;
        GuestAddr kthread = 0;
        GuestAddr stackBase = 0;
        GuestSize stackSize = 0;
        std::shared_ptr<KThread> object;
        std::unordered_map<uint32_t, uint64_t> tls;
    };

    // Thread terminada por ExTerminateThread: desenrola a thread host ate ThreadProc.
    struct ThreadExit {
        uint32_t code;
    };

    std::mutex& Lock();
    std::condition_variable& Cv();

    // Thread atual; cria uma entrada sob demanda para threads host que nao nasceram de ExCreateThread
    // (callbacks da GPU, por exemplo).
    ThreadInfo& CurrentThread();

    // Aloca PCR, KTHREAD e (se stackSize != 0) a stack do guest e prepara o contexto PPC da thread.
    void InitThreadContext(ThreadInfo& info, PPCContext& ctx, GuestSize stackSize);

    // Handles. Os valores seguem a faixa de handles de kernel do Xbox 360 (0xF8000000+).
    uint32_t AddHandle(std::shared_ptr<KObject> object);
    std::shared_ptr<KObject> LookupHandle(uint32_t handle);
    bool RemoveHandle(uint32_t handle);

    // Objeto de dispatcher que vive no guest (KEVENT embutido, ou criado por ObReferenceObjectByHandle).
    // Cria o objeto host na primeira referencia a partir do header. Retorna nullptr se invalido.
    std::shared_ptr<KObject> LookupGuestObject(uint8_t* base, uint32_t ptr);
    // Devolve (alocando se preciso) o ponteiro do guest que representa o objeto.
    uint32_t GuestPointerOf(uint8_t* base, const std::shared_ptr<KObject>& object);

    // timeout100ns: nullptr = infinito; negativo = relativo (unidades de 100 ns); positivo = FILETIME absoluto.
    // Retorna STATUS_WAIT_0 + indice, kTimeout ou kInvalidHandle.
    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KObject>>& objects, bool waitAll, const int64_t* timeout100ns);

    // Sinalizacao (adquirem o lock global).
    uint32_t SetEvent(KEvent& event, uint8_t* base);   // devolve o estado anterior
    uint32_t ResetEvent(KEvent& event, uint8_t* base); // devolve o estado anterior
    void NotifyStateChanged();

    // Cria e inicia uma thread de guest. Retorna o objeto (ja registrado) ou nullptr.
    struct ThreadStart {
        GuestAddr entry = 0;         // funcao a executar (r3 = argumento)
        GuestAddr startupThunk = 0;  // xapi_thread_startup: se != 0 roda startupThunk(entry, argument)
        uint32_t argument = 0;
        GuestSize stackSize = 0;
        bool suspended = false;
    };
    std::shared_ptr<KThread> StartGuestThread(uint8_t* base, const ThreadStart& start);
    // Libera uma thread criada suspensa. Retorna a contagem anterior.
    uint32_t ResumeThread(KThread& thread);

    // Dispositivos montados pelo XAM (XamContentCreateEx): "nome:" -> pasta do host.
    void MountDevice(const std::string& name, const std::filesystem::path& hostPath);
    void UnmountDevice(const std::string& name);
    std::filesystem::path ContentRoot();  // save/content, raiz dos pacotes de conteudo (saves)

    // Pool do kernel (ExAllocatePool*, XamAlloc, strings do Rtl): blocos pequenos nao sao liberados.
    GuestAddr PoolAlloc(uint32_t size);

    // Conversoes de tempo: FILETIME (100 ns desde 1601) e tempo do host.
    uint64_t NowFileTime();
    uint64_t TimebaseTicks();  // 50 MHz

}  // namespace Kernel
