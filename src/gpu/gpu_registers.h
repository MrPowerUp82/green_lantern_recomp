#pragma once

#include <cstdint>

// Registradores da GPU Xenos usados pelo command processor. Os indices sao em dwords;
// a pagina MMIO do guest mapeia o registrador N em kMmioBase + N * 4.
namespace Gpu::Reg {
    constexpr uint32_t kRegisterCount = 0x5003;
    constexpr uint32_t kMmioBase = 0x7FC80000;
    constexpr uint32_t kMmioSize = 0x10000;

    constexpr uint32_t kCpRbWptr = 0x01C5;
    constexpr uint32_t kScratchUmsk = 0x01DC;
    constexpr uint32_t kScratchAddr = 0x01DD;
    constexpr uint32_t kScratchReg0 = 0x0578;
    constexpr uint32_t kScratchReg7 = 0x057F;
    constexpr uint32_t kCoherStatusHost = 0x0A31;
    constexpr uint32_t kVgtEventInitiator = 0x21F9;
    constexpr uint32_t kShaderConstantFetch00_0 = 0x4800;

    // Registradores que o D3D le na pagina MMIO durante a inicializacao e no vblank.
    constexpr uint32_t kRbEdramTiming = 0x0F00;
    constexpr uint32_t kRbBcControl = 0x0F01;
    constexpr uint32_t kD1ModeVCounter = 0x194C;
    constexpr uint32_t kInterruptStatus = 0x1951;
    constexpr uint32_t kD1ModeViewportSize = 0x1961;

    constexpr uint32_t MmioAddress(uint32_t reg) { return kMmioBase + reg * 4; }
}
