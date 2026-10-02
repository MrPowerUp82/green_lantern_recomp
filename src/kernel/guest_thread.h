#pragma once

#include "types.h"

namespace Kernel {
    // Thread principal do jogo: roda o entry point recompilado (_xstart = 0x822FC750)
    // em uma thread host dedicada, com stack host grande (o codigo recompilado usa recursao nativa).
    class GuestThread {
    public:
        static constexpr GuestAddr kEntryPoint = 0x822FC750;
        static constexpr GuestAddr kStackBase = 0x70000000;
        static constexpr GuestSize kStackSize = 0x01000000; // 16 MB (ja commitado em MemoryManager)
        static constexpr GuestSize kPcrSize = 0x1000;

        // Cria e inicia a thread principal do guest. Retorna false em falha.
        static bool StartMain();

        // true enquanto a thread principal do guest estiver executando
        static bool IsRunning();

        // Aguarda o termino (timeoutMs = 0xFFFFFFFF para esperar indefinidamente)
        static bool Join(uint32_t timeoutMs);
    };
}
