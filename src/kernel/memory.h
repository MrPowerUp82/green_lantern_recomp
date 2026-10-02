#pragma once

#include "types.h"
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

namespace Kernel {
    class MemoryManager {
    public:
        static bool Initialize();
        static void Shutdown();

        // Ponteiro base para a memória virtual do console (4 GB)
        static uint8_t* GetBase() { return s_baseMemory; }

        // Carrega as seções do PE descompactado na memória virtual do guest (0x82000000)
        static bool LoadExecutableImage(const std::filesystem::path& unpackedPePath);

        // Inicializa a tabela de dispatch para saltos indiretos PPC (mtctr / bctrl)
        static void InitializeFunctionDispatchTable();

        // Alocações no espaço de endereçamento de 32 bits do console
        static GuestAddr AllocateVirtual(GuestSize size, uint32_t protection = 0x04 /* PAGE_READWRITE */);
        static void FreeVirtual(GuestAddr address);

        // Acesso direto com segurança
        template<typename T>
        static T* TranslateGuest(GuestAddr addr) {
            if (!s_baseMemory) return nullptr;
            return reinterpret_cast<T*>(s_baseMemory + addr);
        }

    private:
        static uint8_t* s_baseMemory;
        static size_t s_allocatedSize;
    };
}
