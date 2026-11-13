// Modulo XEX do jogo (cabecalho, handle do modulo, IAT e variaveis do kernel) e imports Xex*.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"
#include "kernel/xex_module.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Kernel;

namespace {
    GuestAddr g_headerBase = 0;
    uint32_t g_headerSize = 0;
    GuestAddr g_ldrEntry = 0;
    GuestAddr g_timeStampBundle = 0;
    std::atomic<bool> g_clockRunning{false};
    std::thread g_clockThread;

    uint32_t Ld32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
    void St64(uint8_t* base, uint32_t addr, uint64_t value) { *reinterpret_cast<uint64_t*>(base + addr) = Endian::Swap64(value); }

    // Armazenamento das variaveis exportadas pelo kernel. Devolve o endereco da variavel; o valor
    // inicial segue o que o Xbox 360 fornece (alguns valores copiados do Xenia).
    GuestAddr MakeVariable(uint8_t* base, const std::string& name) {
        const GuestAddr address = MemoryManager::AllocateAligned(0x1000, 0x1000, false);
        if (!address) return 0;

        if (name == "__imp__KeTimeStampBundle") {
            g_timeStampBundle = address;
        } else if (name == "__imp__XboxKrnlVersion") {
            // {major u16, minor u16, build u16, qfe u16}: kernel 2.0.17559 (versao de retail tardia)
            base[address + 1] = 2;
            base[address + 4] = static_cast<uint8_t>(17559 >> 8);
            base[address + 5] = static_cast<uint8_t>(17559);
        } else if (name == "__imp__XboxHardwareInfo") {
            St32(base, address + 0x00, 0x00000020);  // flags (placa-mae retail)
            base[address + 4] = 6;                   // processadores
        } else if (name == "__imp__VdGpuClockInMHz") {
            St32(base, address, 500);
        } else if (name == "__imp__XexExecutableModuleHandle") {
            St32(base, address, g_ldrEntry);
        }
        return address;
    }

    void UpdateTimeStampBundle(uint8_t* base) {
        using namespace std::chrono;
        static const auto start = steady_clock::now();
        const uint64_t elapsed100ns = duration_cast<nanoseconds>(steady_clock::now() - start).count() / 100;
        St64(base, g_timeStampBundle + 0x00, elapsed100ns);           // interrupt_time
        St64(base, g_timeStampBundle + 0x08, NowFileTime());          // system_time
        St32(base, g_timeStampBundle + 0x10, static_cast<uint32_t>(elapsed100ns / 10000));  // tick_count (ms)
    }
}

namespace Kernel {

    bool LoadXexModule(const std::filesystem::path& xexFile, const std::filesystem::path& importsFile) {
        uint8_t* base = MemoryManager::GetBase();
        if (!base) return false;

        // 1. Cabecalho do XEX no guest.
        std::ifstream xex(xexFile, std::ios::binary);
        if (!xex) {
            std::cerr << "[Xex] nao foi possivel abrir " << xexFile.string() << std::endl;
            return false;
        }
        uint8_t prefix[16];
        xex.read(reinterpret_cast<char*>(prefix), sizeof(prefix));
        g_headerSize = Endian::ReadBe32(prefix + 8);
        if (g_headerSize < 0x20 || g_headerSize > 0x100000) {
            std::cerr << "[Xex] cabecalho invalido em " << xexFile.string() << std::endl;
            return false;
        }
        g_headerBase = MemoryManager::AllocateAligned(g_headerSize, 0x1000, false);
        xex.seekg(0);
        xex.read(reinterpret_cast<char*>(base + g_headerBase), g_headerSize);

        // 2. LDR_DATA_TABLE_ENTRY do modulo.
        g_ldrEntry = MemoryManager::AllocateAligned(0x100, 0x1000, false);
        St32(base, g_ldrEntry + 0x18, PPC_IMAGE_BASE);        // dll_base
        St32(base, g_ldrEntry + 0x1C, PPC_IMAGE_BASE);        // image_base
        St32(base, g_ldrEntry + 0x20, PPC_IMAGE_SIZE);        // image_size
        St32(base, g_ldrEntry + 0x38, PPC_IMAGE_SIZE);        // full_image_size
        St32(base, g_ldrEntry + 0x3C, 0x822FC750);            // entry_point
        St32(base, g_ldrEntry + 0x58, g_headerBase);          // xex_header_base

        // 3. IAT: variaveis do kernel e enderecos de thunks.
        std::ifstream imports(importsFile);
        if (!imports) {
            std::cerr << "[Xex] tabela de imports ausente: " << importsFile.string()
                      << " (gere com tools/dump_xex_image)" << std::endl;
            return false;
        }
        size_t functions = 0, variables = 0;
        std::string line;
        while (std::getline(imports, line)) {
            std::istringstream parts(line);
            std::string kind, record, thunk, name;
            parts >> kind >> record;
            if (kind == "func") {
                parts >> thunk >> name;
                St32(base, static_cast<uint32_t>(std::stoul(record, nullptr, 16)), static_cast<uint32_t>(std::stoul(thunk, nullptr, 16)));
                ++functions;
            } else if (kind == "var") {
                parts >> name;
                St32(base, static_cast<uint32_t>(std::stoul(record, nullptr, 16)), MakeVariable(base, name));
                ++variables;
            }
        }
        std::cout << "[Xex] cabecalho em 0x" << std::hex << g_headerBase << ", modulo em 0x" << g_ldrEntry << std::dec
                  << ", IAT: " << functions << " funcoes, " << variables << " variaveis" << std::endl;

        if (const GuestAddr exec = XexHeaderField(0x00040006)) {
            std::cout << "[Xex] title id " << std::hex << Ld32(base, exec + 0x0C) << ", media id " << Ld32(base, exec) << std::dec << std::endl;
        }

        // 4. Relogio do kernel.
        if (g_timeStampBundle) {
            UpdateTimeStampBundle(base);
            g_clockRunning = true;
            g_clockThread = std::thread([base] {
                while (g_clockRunning) {
                    UpdateTimeStampBundle(base);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
        return true;
    }

    GuestAddr XexHeaderField(uint32_t key) {
        uint8_t* base = MemoryManager::GetBase();
        if (!base || !g_headerBase) return 0;
        const uint32_t count = Ld32(base, g_headerBase + 0x14);
        for (uint32_t i = 0; i < count && 0x18 + i * 8 + 8 <= g_headerSize; i++) {
            const GuestAddr entry = g_headerBase + 0x18 + i * 8;
            if (Ld32(base, entry) != key) continue;
            // Chaves terminadas em 00/01 guardam o valor no proprio registro; as demais apontam para dados.
            if ((key & 0xFF) <= 1) return entry + 4;
            return g_headerBase + Ld32(base, entry + 4);
        }
        return 0;
    }

    GuestAddr XexModuleHandle() { return g_ldrEntry; }

    void ShutdownXexModule() {
        g_clockRunning = false;
        if (g_clockThread.joinable()) g_clockThread.join();
    }

}  // namespace Kernel

// r3 = base do cabecalho do XEX, r4 = chave -> ponteiro para o valor, ou 0
KERNEL_FUNC(__imp__RtlImageXexHeaderField) {
    (void)base;
    ctx.r3.u64 = XexHeaderField(ctx.r4.u32);
}

// r3 = nome do modulo (char*, nulo = o proprio), r4 = handle*
KERNEL_FUNC(__imp__XexGetModuleHandle) {
    const uint32_t namePtr = ctx.r3.u32;
    const bool self = namePtr == 0 || std::strstr(reinterpret_cast<const char*>(base + namePtr), "default") != nullptr;
    if (!self) { ctx.r3.u64 = 0xC0000135; return; }  // STATUS_DLL_NOT_FOUND
    if (ctx.r4.u32) St32(base, ctx.r4.u32, g_ldrEntry);
    ctx.r3.u64 = 0;
}

// r3 = privilegio -> 1 se o titulo o tem
KERNEL_FUNC(__imp__XexCheckExecutablePrivilege) {
    (void)base;
    const uint32_t privilege = ctx.r3.u32;
    const GuestAddr flags = XexHeaderField(0x00030000);  // XEX_HEADER_SYSTEM_FLAGS
    ctx.r3.u64 = (flags && privilege < 32) ? ((Endian::ReadBe32(MemoryManager::GetBase() + flags) >> privilege) & 1) : 0;
}

// r3 = ponteiro de saida: recebe o endereco do XEX_EXECUTION_ID do titulo
KERNEL_FUNC(__imp__XamGetExecutionId) {
    if (ctx.r3.u32) St32(base, ctx.r3.u32, XexHeaderField(0x00040006));
    ctx.r3.u64 = 0;
}
