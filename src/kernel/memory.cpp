#include "memory.h"
#include "../recompiled/ppc_recomp_shared.h"
#include <iostream>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Kernel {
    uint8_t* MemoryManager::s_baseMemory = nullptr;
    size_t MemoryManager::s_allocatedSize = 0;

    bool MemoryManager::Initialize() {
        if (s_baseMemory != nullptr) return true;

        // Reservar o espaço virtual completo de 4 GB (32-bit address space)
        s_allocatedSize = 0x100000000ull; // 4 GB

#if defined(_WIN32)
        s_baseMemory = static_cast<uint8_t*>(VirtualAlloc(
            nullptr,
            s_allocatedSize,
            MEM_RESERVE,
            PAGE_READWRITE
        ));

        if (!s_baseMemory) {
            std::cerr << "[Memory] Falha crítica ao reservar 4 GB de memória virtual do console." << std::endl;
            return false;
        }

        // 1. Commitar a região do executável e dados do título (0x82000000 - 0x84000000, 32 MB)
        void* titleRegion = VirtualAlloc(
            s_baseMemory + 0x82000000,
            0x02000000, // 32 MB
            MEM_COMMIT,
            PAGE_READWRITE
        );

        // 2. Commitar a região da tabela de despacho de funções (0x82E00000 em diante)
        void* dispatchRegion = VirtualAlloc(
            s_baseMemory + PPC_IMAGE_BASE + PPC_IMAGE_SIZE,
            0x02000000, // 32 MB para tabela de ponteiros
            MEM_COMMIT,
            PAGE_READWRITE
        );

        // 3. Commitar região de Stack padrão (0x70000000 - 0x71000000, 16 MB)
        void* stackRegion = VirtualAlloc(
            s_baseMemory + 0x70000000,
            0x01000000, // 16 MB Stack
            MEM_COMMIT,
            PAGE_READWRITE
        );

        // 4. Commitar região de Heap inicial (0x40000000 - 0x50000000, 256 MB)
        void* heapRegion = VirtualAlloc(
            s_baseMemory + 0x40000000,
            0x10000000, // 256 MB Heap
            MEM_COMMIT,
            PAGE_READWRITE
        );

        if (!titleRegion || !dispatchRegion || !stackRegion || !heapRegion) {
            std::cerr << "[Memory] Falha ao comitar páginas de memória essenciais." << std::endl;
            return false;
        }
#endif

        std::cout << "[Memory] Espaço de 4 GB do Xbox 360 inicializado com sucesso em base: " 
                  << static_cast<void*>(s_baseMemory) << std::endl;

        return true;
    }

    void MemoryManager::Shutdown() {
        if (s_baseMemory) {
#if defined(_WIN32)
            VirtualFree(s_baseMemory, 0, MEM_RELEASE);
#endif
            s_baseMemory = nullptr;
            s_allocatedSize = 0;
            std::cout << "[Memory] Memória virtual liberada." << std::endl;
        }
    }

    bool MemoryManager::LoadExecutableImage(const std::filesystem::path& unpackedPePath) {
        if (!s_baseMemory) {
            if (!Initialize()) return false;
        }

        std::ifstream file(unpackedPePath, std::ios::binary);
        if (!file) {
            std::cerr << "[Memory] Não foi possível abrir o PE: " << unpackedPePath.string() << std::endl;
            return false;
        }

        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (buffer.size() < 0x200) {
            std::cerr << "[Memory] Arquivo PE muito pequeno ou inválido." << std::endl;
            return false;
        }

        uint32_t peOffset = *reinterpret_cast<uint32_t*>(&buffer[0x3C]);
        uint16_t numSections = *reinterpret_cast<uint16_t*>(&buffer[peOffset + 6]);
        uint16_t optHdrSize = *reinterpret_cast<uint16_t*>(&buffer[peOffset + 20]);
        size_t secStart = peOffset + 24 + optHdrSize;

        std::cout << "[Memory] Carregando " << numSections << " seções do PE na memória virtual..." << std::endl;

        for (size_t i = 0; i < numSections; i++) {
            size_t entry = secStart + i * 40;
            char name[9] = { 0 };
            std::memcpy(name, &buffer[entry], 8);

            uint32_t vSize = *reinterpret_cast<uint32_t*>(&buffer[entry + 8]);
            uint32_t vAddr = *reinterpret_cast<uint32_t*>(&buffer[entry + 12]);
            uint32_t rawSize = *reinterpret_cast<uint32_t*>(&buffer[entry + 16]);
            uint32_t rawPtr = *reinterpret_cast<uint32_t*>(&buffer[entry + 20]);

            uint8_t* dest = s_baseMemory + 0x82000000 + vAddr;

            size_t copySize = std::min<size_t>(rawSize, buffer.size() - rawPtr);
            std::memcpy(dest, &buffer[rawPtr], copySize);

            // Preencher com zeros se o VirtualSize for maior que RawData
            if (vSize > copySize) {
                std::memset(dest + copySize, 0, vSize - copySize);
            }

            std::cout << "  [Seção " << name << "] Mapeada em 0x" 
                      << std::hex << (0x82000000 + vAddr) 
                      << " (Tamanho: 0x" << vSize << " bytes)" << std::dec << std::endl;
        }

        std::cout << "[Memory] Imagem do jogo totalmente mapeada na memória virtual do console." << std::endl;
        return true;
    }

    void MemoryManager::InitializeFunctionDispatchTable() {
        if (!s_baseMemory) return;

        std::cout << "[Memory] Populando tabela de despacho de funções (PPCFuncMappings)..." << std::endl;
        size_t count = 0;
        for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++) {
            GuestAddr guest = static_cast<GuestAddr>(PPCFuncMappings[i].guest);
            PPCFunc* host = PPCFuncMappings[i].host;

            PPC_LOOKUP_FUNC(s_baseMemory, guest) = host;
            count++;
        }

        std::cout << "[Memory] " << count << " funções registradas com sucesso na tabela de despacho indireto." << std::endl;
    }

    GuestAddr MemoryManager::AllocateVirtual(GuestSize size, uint32_t protection) {
        (void)protection;
        static GuestAddr s_heapCursor = 0x40000000;
        GuestAddr allocated = s_heapCursor;
        // Alinhamento em 64KB (padrão de página do Xenon)
        size_t alignedSize = (size + 0xFFFF) & ~0xFFFF;
        s_heapCursor += static_cast<GuestAddr>(alignedSize);
        return allocated;
    }

    void MemoryManager::FreeVirtual(GuestAddr address) {
        (void)address;
    }
}
