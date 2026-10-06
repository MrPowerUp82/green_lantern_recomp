#pragma once

#include <atomic>
#include <cstdint>

namespace Gpu {
    // Simula o D3D do jogo sem o jogo: inicializa o ring pelos imports Vd*, publica `frames`
    // swaps (VdSwap + WPTR) e espera o command processor consumi-los. Exige GpuSystem inicializado.
    // Retorna true se o write-back do RPTR alcancou o WPTR a tempo (2 s). `cancel`, se dado, interrompe
    // a publicacao de novos frames quando vira true (ex.: janela fechada).
    bool RunSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel = nullptr);
}
