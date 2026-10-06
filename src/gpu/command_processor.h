#pragma once

#include "gpu/gpu_backend.h"
#include "gpu/gpu_registers.h"
#include "gpu/guest_memory.h"
#include "gpu/packet_stream.h"
#include "gpu/pm4.h"

#include <array>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace Gpu {
    // Destino das interrupcoes geradas pelo CP (pacote INTERRUPT).
    class IInterruptSink {
    public:
        virtual ~IInterruptSink() = default;
        virtual void Dispatch(uint32_t source, uint32_t cpu) = 0;
    };

    struct CpStats {
        uint64_t packets = 0;
        uint64_t swaps = 0;
        uint64_t draws = 0;      // DRAW_INDX* contados, nao executados no M1
        uint64_t unhandled = 0;  // opcodes type 3 pulados pelo tamanho
        uint64_t malformed = 0;  // pacotes invalidos (descartados ate o WPTR)
    };

    // Emula o command processor do Xenos: consome o ring buffer PM4 escrito pelo D3D do jogo.
    class CommandProcessor {
    public:
        CommandProcessor(uint8_t* base, IGpuBackend* backend, IInterruptSink* interrupts);
        ~CommandProcessor();
        CommandProcessor(const CommandProcessor&) = delete;
        CommandProcessor& operator=(const CommandProcessor&) = delete;

        // sizeLog2: o ring tem (1 << (sizeLog2 + 3)) bytes (convencao de VdInitializeRingBuffer).
        void InitializeRingBuffer(GuestAddr ptr, uint32_t sizeLog2);
        // O RPTR e escrito (big-endian) em ptr depois de cada lote. blockSizeLog2 nao e usado.
        void EnableReadPointerWriteBack(GuestAddr ptr, uint32_t blockSizeLog2);

        void Start();
        void Stop();

        // Executa o que o guest publicou no WPTR (pagina MMIO). true se processou algo.
        // E o que a thread do CP chama em loop; os testes chamam direto.
        bool PumpOnce();

        void IncrementCounter() { m_counter.fetch_add(1, std::memory_order_relaxed); }
        uint32_t Counter() const { return m_counter.load(std::memory_order_relaxed); }
        uint32_t RegisterValue(uint32_t index) const { return index < Reg::kRegisterCount ? m_regs[index].load(std::memory_order_relaxed) : 0; }
        uint32_t ReadIndex() const { return m_readIndex.load(std::memory_order_acquire); }
        CpStats Stats() const;

    private:
        static constexpr int kMaxIndirectDepth = 8;

        void WorkerMain();
        uint32_t ExecutePrimaryBuffer(GuestAddr ringBase, uint32_t ringDwords, uint32_t readIndex, uint32_t writeIndex);
        bool ExecuteIndirectBuffer(GuestAddr ptr, uint32_t countDwords, int depth);
        bool ExecutePacket(PacketStream& stream, int depth);
        bool ExecuteType0(PacketStream& stream, const Pm4::Header& header);
        bool ExecuteType3(PacketStream& stream, const Pm4::Header& header, int depth);
        bool ExecuteWaitRegMem(const uint32_t* d, uint32_t n);
        bool ExecuteCondWrite(const uint32_t* d, uint32_t n);
        bool ExecuteEventWriteExt(const uint32_t* d, uint32_t n);
        bool ExecuteLoadAluConstant(const uint32_t* d, uint32_t n);

        void WriteRegister(uint32_t index, uint32_t value);
        bool LoadSwapped(GuestAddr addrWithMode, uint32_t* out) const;
        bool StoreSwapped(GuestAddr addrWithMode, uint32_t value) const;
        void WriteBackReadPointer();
        void LogOnce(std::set<uint32_t>& seen, uint32_t key, const char* what, uint32_t detail);

        GuestMemory m_memory;
        IGpuBackend* m_backend;
        IInterruptSink* m_interrupts;

        std::array<std::atomic<uint32_t>, Reg::kRegisterCount> m_regs{};

        std::mutex m_ringMutex;  // protege base/tamanho do ring contra re-inicializacao pelo guest
        GuestAddr m_ringBase = 0;
        uint32_t m_ringDwords = 0;
        std::atomic<uint32_t> m_readIndex{0};
        std::atomic<GuestAddr> m_rptrWriteback{0};

        std::atomic<uint32_t> m_counter{0};
        std::atomic<bool> m_stop{false};
        std::thread m_thread;
        std::mutex m_waitMutex;
        std::condition_variable m_waitCv;

        std::atomic<uint64_t> m_packets{0}, m_swaps{0}, m_draws{0}, m_unhandled{0}, m_malformed{0};
        std::set<uint32_t> m_loggedOpcodes;
        std::set<uint32_t> m_loggedMisc;
    };
}
