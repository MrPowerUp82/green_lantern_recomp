#include "gpu/command_processor.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

namespace Gpu {
    namespace {
        constexpr uint32_t kMaxRingSizeLog2 = 24;

        // Base do register file para cada tipo de constante em SET_CONSTANT / LOAD_ALU_CONSTANT.
        bool ConstantBase(uint32_t type, uint32_t* base) {
            switch (type) {
            case 0: *base = 0x4000; return true;  // ALU
            case 1: *base = 0x4800; return true;  // FETCH
            case 2: *base = 0x4900; return true;  // BOOL
            case 3: *base = 0x4908; return true;  // LOOP
            case 4: *base = 0x2000; return true;  // REGISTERS
            default: return false;
            }
        }

        bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
            switch (function & 7) {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }
    }

    CommandProcessor::CommandProcessor(uint8_t* base, IGpuBackend* backend, IInterruptSink* interrupts)
        : m_memory(base), m_backend(backend), m_interrupts(interrupts) {}

    CommandProcessor::~CommandProcessor() { Stop(); }

    void CommandProcessor::InitializeRingBuffer(GuestAddr ptr, uint32_t sizeLog2) {
        if (!m_memory.Base() || (ptr & 3) || sizeLog2 > kMaxRingSizeLog2 || !GuestMemory::Valid(ptr, uint64_t(1) << (sizeLog2 + 3))) {
            std::cerr << "[GPU] VdInitializeRingBuffer invalido: ptr=0x" << std::hex << ptr
                      << " size_log2=" << std::dec << sizeLog2 << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_ringBase = ptr;
        m_ringDwords = (uint32_t(1) << (sizeLog2 + 3)) / 4;
        m_readIndex.store(0, std::memory_order_release);
    }

    void CommandProcessor::EnableReadPointerWriteBack(GuestAddr ptr, uint32_t) {
        m_rptrWriteback.store(ptr, std::memory_order_release);
    }

    void CommandProcessor::Start() {
        if (m_thread.joinable()) return;
        m_stop.store(false);
        m_thread = std::thread([this] { WorkerMain(); });
    }

    void CommandProcessor::Stop() {
        {
            std::lock_guard<std::mutex> lock(m_waitMutex);
            m_stop.store(true);
        }
        m_waitCv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    CpStats CommandProcessor::Stats() const {
        CpStats s;
        s.packets = m_packets.load();
        s.swaps = m_swaps.load();
        s.draws = m_draws.load();
        s.unhandled = m_unhandled.load();
        s.malformed = m_malformed.load();
        return s;
    }

    void CommandProcessor::WorkerMain() {
        uint32_t idle = 0;
        while (!m_stop.load()) {
            if (PumpOnce()) {
                idle = 0;
                continue;
            }
            // Sem comandos: spin curto, depois espera de ~100 us para nao ocupar um nucleo.
            if (++idle < 200) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    bool CommandProcessor::PumpOnce() {
        // Serializa o lote com uma eventual reinicializacao do ring pelo guest.
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const GuestAddr ringBase = m_ringBase;
        const uint32_t ringDwords = m_ringDwords;
        if (ringBase == 0 || ringDwords == 0) return false;

        const uint32_t writeIndex = m_memory.LoadBe32(Reg::MmioAddress(Reg::kCpRbWptr)) % ringDwords;
        const uint32_t readIndex = m_readIndex.load(std::memory_order_acquire);
        if (readIndex == writeIndex) return false;

        const uint32_t newRead = ExecutePrimaryBuffer(ringBase, ringDwords, readIndex, writeIndex);
        m_readIndex.store(newRead, std::memory_order_release);
        WriteBackReadPointer();
        return true;
    }

    uint32_t CommandProcessor::ExecutePrimaryBuffer(GuestAddr ringBase, uint32_t ringDwords,
                                                    uint32_t readIndex, uint32_t writeIndex) {
        const uint32_t pending = (writeIndex + ringDwords - readIndex) % ringDwords;
        PacketStream stream(m_memory, ringBase, ringDwords, readIndex, pending);

        while (stream.Remaining() > 0 && !m_stop.load()) {
            if (!ExecutePacket(stream, 0)) {
                if (!m_stop.load()) {
                    m_malformed.fetch_add(1);
                    LogOnce(m_loggedMisc, 0xFFFF0000u, "pacote PM4 invalido no ring; descartando ate o WPTR, indice",
                            stream.ReadIndex());
                }
                break;  // descarta o resto ate o WPTR
            }
        }
        return writeIndex;
    }

    bool CommandProcessor::ExecuteIndirectBuffer(GuestAddr ptr, uint32_t countDwords, int depth) {
        if (depth >= kMaxIndirectDepth || (ptr & 3)) return false;
        if (countDwords == 0) return true;
        if (!GuestMemory::Valid(ptr, uint64_t(countDwords) * 4)) return false;

        PacketStream stream(m_memory, ptr, countDwords, 0, countDwords);
        while (stream.Remaining() > 0 && !m_stop.load()) {
            if (!ExecutePacket(stream, depth + 1)) return false;
        }
        return true;
    }

    bool CommandProcessor::ExecutePacket(PacketStream& stream, int depth) {
        uint32_t raw;
        if (!stream.Read(&raw)) return false;
        m_packets.fetch_add(1, std::memory_order_relaxed);

        if (raw == 0) return true;  // padding: nao carrega payload

        const Pm4::Header header = Pm4::DecodeHeader(raw);
        switch (header.type) {
        case 0: return ExecuteType0(stream, header);
        case 2: return true;
        case 3: return ExecuteType3(stream, header, depth);
        default: return false;  // tipo 1 nao e emitido pelo D3D do 360
        }
    }

    bool CommandProcessor::ExecuteType0(PacketStream& stream, const Pm4::Header& header) {
        if (stream.Remaining() < header.count) return false;
        for (uint32_t i = 0; i < header.count; i++) {
            uint32_t value;
            if (!stream.Read(&value)) return false;
            WriteRegister(header.oneReg ? header.baseIndex : header.baseIndex + i, value);
        }
        return true;
    }

    bool CommandProcessor::ExecuteType3(PacketStream& stream, const Pm4::Header& header, int depth) {
        const uint32_t n = header.count;
        if (stream.Remaining() < n) return false;

        // O payload e lido por inteiro antes do handler: o ring fica consistente mesmo para
        // opcodes desconhecidos ou que consomem menos dwords do que o pacote declara.
        uint32_t small[64];
        std::vector<uint32_t> large;
        uint32_t* d = small;
        if (n > 64) {
            large.resize(n);
            d = large.data();
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!stream.Read(&d[i])) return false;
        }

        switch (header.opcode) {
        case Pm4::kNop:
        case Pm4::kMeInit:
            return true;

        case Pm4::kInterrupt: {
            if (n < 1) return false;
            if (m_interrupts) {
                for (uint32_t cpu = 0; cpu < 6; cpu++) {
                    if (d[0] & (1u << cpu)) m_interrupts->Dispatch(1, cpu);
                }
            }
            return true;
        }

        case Pm4::kXeSwap: {
            if (n < 4 || d[0] != Pm4::kSwapSignature) return false;
            if (!m_backend) return false;
            m_backend->Swap(d[1], d[2], d[3]);
            m_swaps.fetch_add(1);
            IncrementCounter();
            return true;
        }

        case Pm4::kIndirectBuffer:
        case Pm4::kIndirectBufferPfd: {
            if (n < 2) return false;
            const GuestAddr ptr = d[0];
            const uint32_t count = d[1] & 0xFFFFF;
            return ExecuteIndirectBuffer(ptr, count, depth);
        }

        case Pm4::kWaitRegMem: return ExecuteWaitRegMem(d, n);
        case Pm4::kCondWrite: return ExecuteCondWrite(d, n);
        case Pm4::kEventWriteExt: return ExecuteEventWriteExt(d, n);
        case Pm4::kLoadAluConstant: return ExecuteLoadAluConstant(d, n);

        case Pm4::kRegRmw: {
            if (n < 3) return false;
            const uint32_t index = d[0] & 0x1FFF;
            uint32_t value = m_regs[index];
            value &= ((d[0] >> 31) & 1u) ? m_regs[d[1] & 0x1FFF].load() : d[1];
            value |= ((d[0] >> 30) & 1u) ? m_regs[d[2] & 0x1FFF].load() : d[2];
            WriteRegister(index, value);
            return true;
        }

        case Pm4::kRegToMem: {
            if (n < 2 || d[0] >= Reg::kRegisterCount) return false;
            return StoreSwapped(d[1], m_regs[d[0]]);
        }

        case Pm4::kMemWrite: {
            if (n < 1) return false;
            const SwapMode mode = SwapMode(d[0] & 3);
            const GuestAddr base = d[0] & ~GuestAddr(3);
            for (uint32_t i = 0; i + 1 < n; i++) {
                const uint64_t addr = uint64_t(base) + uint64_t(i) * 4;
                if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
                m_memory.StoreRaw32(GuestAddr(addr), GpuSwap(d[1 + i], mode));
            }
            return true;
        }

        case Pm4::kEventWrite: {
            if (n < 1) return false;
            WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);
            return true;
        }

        case Pm4::kEventWriteShd: {
            if (n < 3) return false;
            WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);
            const uint32_t value = ((d[0] >> 31) & 1u) ? Counter() : d[2];
            return StoreSwapped(d[1], value);
        }

        case Pm4::kSetConstant: {
            if (n < 1) return false;
            uint32_t base;
            if (!ConstantBase((d[0] >> 16) & 0xFF, &base)) return true;  // tipo desconhecido: pula
            const uint32_t index = base + (d[0] & 0x7FF);
            for (uint32_t i = 1; i < n; i++) WriteRegister(index + i - 1, d[i]);
            return true;
        }

        case Pm4::kSetConstant2:
        case Pm4::kSetShaderConstants: {
            if (n < 1) return false;
            const uint32_t index = d[0] & 0xFFFF;
            for (uint32_t i = 1; i < n; i++) WriteRegister(index + i - 1, d[i]);
            return true;
        }

        case Pm4::kDrawIndx:
        case Pm4::kDrawIndx2:
            m_draws.fetch_add(1);
            return true;

        default:
            m_unhandled.fetch_add(1);
            LogOnce(m_loggedOpcodes, header.opcode, "opcode PM4 sem suporte no M1 (pulado), opcode", header.opcode);
            return true;
        }
    }

    bool CommandProcessor::ExecuteWaitRegMem(const uint32_t* d, uint32_t n) {
        if (n < 5) return false;
        const uint32_t waitInfo = d[0];
        const uint32_t pollAddr = d[1];
        const uint32_t ref = d[2];
        const uint32_t mask = d[3];
        const uint32_t wait = d[4];

        if ((waitInfo & 0x10) == 0) {
            // Registrador: so o proprio CP altera o register file, entao a condicao nao pode mudar
            // enquanto esperamos. Avalia uma vez (apos o coherency do host) e segue adiante.
            if (pollAddr >= Reg::kRegisterCount) return false;
            if (pollAddr == Reg::kCoherStatusHost && (m_regs[pollAddr] & 0x80000000u)) {
                m_regs[pollAddr] = 0;
            }
            if (!Compare(waitInfo, m_regs[pollAddr] & mask, ref)) {
                LogOnce(m_loggedMisc, 0xFFFF0001u, "WAIT_REG_MEM em registrador nunca satisfeito (ignorado), registrador", pollAddr);
            }
            return true;
        }

        // Memoria: espera ate outra thread (guest) satisfazer a condicao, ou ate Stop().
        while (true) {
            uint32_t value;
            if (!LoadSwapped(pollAddr, &value)) return false;
            if (Compare(waitInfo, value & mask, ref)) return true;
            if (m_stop.load()) return false;
            if (wait >= 0x100) {
                std::unique_lock<std::mutex> lock(m_waitMutex);
                if (m_waitCv.wait_for(lock, std::chrono::milliseconds(wait / 0x100),
                                     [this] { return m_stop.load(); })) return false;
            } else {
                std::this_thread::yield();
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    bool CommandProcessor::ExecuteCondWrite(const uint32_t* d, uint32_t n) {
        if (n < 6) return false;
        const uint32_t waitInfo = d[0];
        uint32_t value;
        if (waitInfo & 0x10) {
            if (!LoadSwapped(d[1], &value)) return false;
        } else {
            if (d[1] >= Reg::kRegisterCount) return false;
            value = m_regs[d[1]];
        }
        if (!Compare(waitInfo, value & d[3], d[2])) return true;

        if (waitInfo & 0x100) return StoreSwapped(d[4], d[5]);
        WriteRegister(d[4], d[5]);
        return true;
    }

    bool CommandProcessor::ExecuteEventWriteExt(const uint32_t* d, uint32_t n) {
        if (n < 2) return false;
        WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);

        // O driver le as extensoes (x/y/z min/max) afetadas pelo ultimo draw; devolvemos a tela toda.
        const GuestAddr addr = d[1] & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 12)) return false;
        const uint16_t extents[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
        for (uint32_t i = 0; i < 6; i++) m_memory.StoreBe16(addr + i * 2, extents[i]);
        return true;
    }

    bool CommandProcessor::ExecuteLoadAluConstant(const uint32_t* d, uint32_t n) {
        if (n < 3) return false;
        // Endereco fisico == virtual neste port (MmGetPhysicalAddress e a identidade): sem mascara.
        const GuestAddr address = d[0];
        if (address & 3) return false;
        const uint32_t size = d[2] & 0xFFF;
        uint32_t base;
        if (!ConstantBase((d[1] >> 16) & 0xFF, &base)) return true;
        const uint32_t index = base + (d[1] & 0x7FF);
        for (uint32_t i = 0; i < size; i++) {
            const uint64_t addr = uint64_t(address) + uint64_t(i) * 4;
            if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
            WriteRegister(index + i, m_memory.LoadBe32(GuestAddr(addr)));
        }
        return true;
    }

    void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
        if (index >= Reg::kRegisterCount) {
            LogOnce(m_loggedMisc, 0xFFFF0002u, "escrita em registrador fora do range (ignorada), indice", index);
            return;
        }
        m_regs[index] = value;

        if (index >= Reg::kScratchReg0 && index <= Reg::kScratchReg7) {
            // Registradores scratch espelhados em memoria quando habilitados em SCRATCH_UMSK.
            const uint32_t slot = index - Reg::kScratchReg0;
            if (m_regs[Reg::kScratchUmsk] & (1u << slot)) {
                const uint64_t addr = uint64_t(m_regs[Reg::kScratchAddr].load()) + slot * 4;
                if (!(addr & 3) && addr <= 0xFFFFFFFFull && GuestMemory::Valid(GuestAddr(addr), 4))
                    m_memory.StoreBe32(GuestAddr(addr), value);
            }
        } else if (index == Reg::kCoherStatusHost) {
            m_regs[index].fetch_or(0x80000000u);  // pendente: o proximo WAIT_REG_MEM sincroniza
        }
    }

    bool CommandProcessor::LoadSwapped(GuestAddr addrWithMode, uint32_t* out) const {
        const GuestAddr addr = addrWithMode & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 4)) return false;
        *out = GpuSwap(m_memory.LoadRaw32(addr), SwapMode(addrWithMode & 3));
        return true;
    }

    bool CommandProcessor::StoreSwapped(GuestAddr addrWithMode, uint32_t value) const {
        const GuestAddr addr = addrWithMode & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 4)) return false;
        m_memory.StoreRaw32(addr, GpuSwap(value, SwapMode(addrWithMode & 3)));
        return true;
    }

    void CommandProcessor::WriteBackReadPointer() {
        const GuestAddr addr = m_rptrWriteback.load(std::memory_order_acquire);
        if ((addr & 3) || !GuestMemory::Valid(addr, 4)) return;
        std::atomic_thread_fence(std::memory_order_release);
        m_memory.StoreBe32(addr, m_readIndex.load(std::memory_order_acquire));
    }

    void CommandProcessor::LogOnce(std::set<uint32_t>& seen, uint32_t key, const char* what, uint32_t detail) {
        if (!seen.insert(key).second) return;
        std::cerr << "[GPU] " << what << " 0x" << std::hex << detail << std::dec << std::endl;
    }
}
