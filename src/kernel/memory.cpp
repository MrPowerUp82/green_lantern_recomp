#include "memory.h"
#include "../recompiled/ppc_recomp_shared.h"
#include "gpu/gpu_registers.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <atomic>
#include <cstring>
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
        // RAM fisica de 512 MB espelhada em 0xA0000000 (paginas de 64 KB), 0xC0000000 (16 MB) e 0xE0000000
        // (4 KB): o jogo acessa a mesma memoria por qualquer uma das tres. Uma secao de arquivo mapeada
        // tres vezes da o alias real. O espaco de 4 GB e reservado em duas partes, deixando o resto livre
        // para as visoes; se a base escolhida nao servir, tenta outra.
        constexpr size_t kPhysicalSize = 0x20000000;
        HANDLE physical = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysicalSize, nullptr);
        if (!physical) {
            std::cerr << "[Memory] Falha ao criar a RAM fisica de 512 MB." << std::endl;
            return false;
        }
        for (int attempt = 0; attempt < 16 && !s_baseMemory; attempt++) {
            void* probe = VirtualAlloc(nullptr, s_allocatedSize, MEM_RESERVE, PAGE_READWRITE);
            if (!probe) break;
            VirtualFree(probe, 0, MEM_RELEASE);
            auto* candidate = static_cast<uint8_t*>(probe);
            if (!VirtualAlloc(candidate, 0xA0000000ull, MEM_RESERVE, PAGE_READWRITE)) continue;
            bool mapped = true;
            for (uint64_t window : {0xA0000000ull, 0xC0000000ull, 0xE0000000ull}) {
                if (!MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, candidate + window)) {
                    mapped = false;
                    break;
                }
            }
            if (!mapped) {
                for (uint64_t window : {0xA0000000ull, 0xC0000000ull, 0xE0000000ull}) UnmapViewOfFile(candidate + window);
                VirtualFree(candidate, 0, MEM_RELEASE);
                continue;
            }
            s_baseMemory = candidate;
        }

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

        // Pagina MMIO: D3D publica WPTR aqui, em big-endian.
        void* gpuMmioRegion = VirtualAlloc(s_baseMemory + Gpu::Reg::kMmioBase,
            Gpu::Reg::kMmioSize, MEM_COMMIT, PAGE_READWRITE);

        // Registradores do XMA (decodificador de audio) em 0x7FEA0000: sem emulacao, so absorvem escritas.
        void* xmaMmioRegion = VirtualAlloc(s_baseMemory + 0x7FEA0000, 0x10000, MEM_COMMIT, PAGE_READWRITE);

        if (!titleRegion || !dispatchRegion || !stackRegion || !heapRegion || !gpuMmioRegion || !xmaMmioRegion) {
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
            for (uint64_t window : {0xA0000000ull, 0xC0000000ull, 0xE0000000ull}) UnmapViewOfFile(s_baseMemory + window);
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

        // Imagem bruta gerada por tools/dump_xex_image: ja esta no layout de memoria do guest (por RVA).
        if (unpackedPePath.extension() == ".image") {
            if (buffer.size() > PPC_IMAGE_SIZE) {
                std::cerr << "[Memory] Imagem maior que PPC_IMAGE_SIZE." << std::endl;
                return false;
            }
            std::memcpy(s_baseMemory + PPC_IMAGE_BASE, buffer.data(), buffer.size());
            std::cout << "[Memory] Imagem do jogo (" << buffer.size() << " bytes) mapeada em 0x"
                      << std::hex << PPC_IMAGE_BASE << std::dec << "." << std::endl;
            return true;
        }

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

    namespace {
        // Heap do guest: lista livre ordenada (inicio -> tamanho) e mapa de blocos usados.
        struct Heap {
            std::mutex lock;
            std::map<GuestAddr, uint32_t> freeList;
            std::map<GuestAddr, uint32_t> used;
            uint32_t pageSize = 0x1000;

            void Init(GuestAddr begin, GuestAddr end, uint32_t page) {
                freeList.clear();
                used.clear();
                freeList[begin] = end - begin;
                pageSize = page;
            }

            GuestAddr Alloc(uint8_t* base, uint32_t size, uint32_t align, GuestAddr fixed) {
                std::lock_guard<std::mutex> guard(lock);
                const uint64_t sz = (uint64_t(size) + pageSize - 1) & ~uint64_t(pageSize - 1);
                if (sz == 0 || sz > 0xFFFFFFFFull) return 0;
                if (align < pageSize) align = pageSize;
                for (auto it = freeList.begin(); it != freeList.end(); ++it) {
                    const uint64_t blockEnd = uint64_t(it->first) + it->second;
                    uint64_t start = (uint64_t(it->first) + align - 1) & ~uint64_t(align - 1);
                    if (fixed) {
                        if (fixed < it->first || uint64_t(fixed) + sz > blockEnd) continue;
                        start = fixed;
                    }
                    if (start + sz > blockEnd) continue;

                    const GuestAddr blockStart = it->first;
                    freeList.erase(it);
                    if (start > blockStart) freeList[blockStart] = GuestAddr(start - blockStart);
                    if (start + sz < blockEnd) freeList[GuestAddr(start + sz)] = GuestAddr(blockEnd - (start + sz));
                    used[GuestAddr(start)] = GuestSize(sz);
#if defined(_WIN32)
                    VirtualAlloc(base + start, sz, MEM_COMMIT, PAGE_READWRITE);
#endif
                    std::memset(base + start, 0, size_t(sz));
                    return GuestAddr(start);
                }
                return 0;
            }

            uint32_t Free(GuestAddr addr) {
                std::lock_guard<std::mutex> guard(lock);
                auto it = used.find(addr);
                if (it == used.end()) return 0;
                const uint32_t size = it->second;
                used.erase(it);
                auto next = freeList.lower_bound(addr);
                GuestAddr start = addr;
                uint32_t len = size;
                if (next != freeList.end() && addr + size == next->first) {
                    len += next->second;
                    next = freeList.erase(next);
                }
                if (next != freeList.begin()) {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == start) {
                        start = prev->first;
                        len += prev->second;
                        freeList.erase(prev);
                    }
                }
                freeList[start] = len;
                return size;
            }

            uint32_t SizeOf(GuestAddr addr) {
                std::lock_guard<std::mutex> guard(lock);
                auto it = used.find(addr);
                if (it != used.end()) return it->second;
                // Endereco no meio de um bloco: devolve o tamanho restante a partir dele.
                auto up = used.upper_bound(addr);
                if (up == used.begin()) return 0;
                --up;
                return addr < up->first + up->second ? up->first + up->second - addr : 0;
            }
        };

        // Heap virtual do titulo (NtAllocateVirtualMemory, stacks, buffers do kernel) e a faixa
        // "fisica" (MmAllocatePhysicalMemoryEx), onde o port usa fisico == virtual.
        constexpr GuestAddr kVirtualHeapBegin = 0x40000000;
        constexpr GuestAddr kVirtualHeapEnd = 0x6F000000;
        constexpr GuestAddr kPhysicalHeapBegin = 0xA0000000;
        constexpr GuestAddr kPhysicalHeapEnd = 0xC0000000;

        Heap& VirtualHeap() {
            static Heap heap;
            static std::once_flag once;
            std::call_once(once, [] { heap.Init(kVirtualHeapBegin, kVirtualHeapEnd, 0x1000); });
            return heap;
        }

        Heap& PhysicalHeap() {
            static Heap heap;
            static std::once_flag once;
            std::call_once(once, [] { heap.Init(kPhysicalHeapBegin, kPhysicalHeapEnd, 0x1000); });
            return heap;
        }
    }

    GuestAddr MemoryManager::AllocateVirtual(GuestSize size, uint32_t protection) {
        (void)protection;
        // Alinhamento em 64KB (padrao de pagina do Xenon)
        return VirtualHeap().Alloc(s_baseMemory, size, 0x10000, 0);
    }

    GuestAddr MemoryManager::AllocateAligned(GuestSize size, uint32_t alignment, bool physical, GuestAddr fixedAddress) {
        Heap& heap = physical ? PhysicalHeap() : VirtualHeap();
        return heap.Alloc(s_baseMemory, size, alignment, fixedAddress);
    }

    GuestSize MemoryManager::AllocationSize(GuestAddr address) {
        if (address >= kPhysicalHeapBegin && address < kPhysicalHeapEnd) return PhysicalHeap().SizeOf(address);
        return VirtualHeap().SizeOf(address);
    }

    void MemoryManager::FreeVirtual(GuestAddr address) {
        if (address >= kPhysicalHeapBegin && address < kPhysicalHeapEnd) PhysicalHeap().Free(address);
        else VirtualHeap().Free(address);
    }
}
