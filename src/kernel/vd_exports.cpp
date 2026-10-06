// Imports Vd* do xboxkrnl.exe: a interface entre a D3D do jogo e a GPU emulada.
// Cada funcao segue a convencao do codigo recompilado: argumentos em r3..r10 (mais a pilha em
// r1 + 0x54 + 8 * n para o 9o argumento em diante) e retorno em r3.

#include "recompiled/ppc_context.h"

#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/pm4.h"

#include <cstring>
#include <iostream>

namespace {
    using Gpu::GpuSystem;

    constexpr uint32_t kDisplayWidth = 1280;
    constexpr uint32_t kDisplayHeight = 720;
    constexpr float kRefreshRate = 60.0f;
    constexpr uint32_t kStackArgBase = 0x54;

    uint32_t FloatBits(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // n-esimo argumento passado pela pilha (0 = o 9o argumento).
    uint32_t StackArg(PPCContext& ctx, uint32_t n) {
        return GpuSystem::Get().Memory().LoadBe32(ctx.r1.u32 + kStackArgBase + n * 8);
    }

    bool Ready(const char* name) {
        if (GpuSystem::Get().IsInitialized()) return true;
        std::cerr << "[Vd] " << name << " chamado antes de GpuSystem::Initialize." << std::endl;
        return false;
    }

    void Zero(GuestAddr addr, uint32_t bytes) {
        std::memset(GpuSystem::Get().Memory().Base() + addr, 0, bytes);
    }

    // struct X_VIDEO_MODE (48 bytes, big-endian), valores iguais aos do Xenia.
    void WriteVideoMode(GuestAddr addr) {
        const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
        Zero(addr, 48);
        mem.StoreBe32(addr + 0x00, kDisplayWidth);
        mem.StoreBe32(addr + 0x04, kDisplayHeight);
        mem.StoreBe32(addr + 0x08, 0);                       // is_interlaced
        mem.StoreBe32(addr + 0x0C, 1);                       // is_widescreen
        mem.StoreBe32(addr + 0x10, 1);                       // is_hi_def
        mem.StoreBe32(addr + 0x14, FloatBits(kRefreshRate)); // refresh_rate
        mem.StoreBe32(addr + 0x18, 1);                       // video_standard (NTSC)
        mem.StoreBe32(addr + 0x1C, 0x4A);                    // unknown_0x8a
        mem.StoreBe32(addr + 0x20, 0x01);                    // unknown_0x01
    }
}

// --- Inicializacao do ring buffer e da GPU ---------------------------------------------------

// r3 = endereco do ring (resultado de MmGetPhysicalAddress), r4 = log2 do tamanho
PPC_FUNC(__imp__VdInitializeRingBuffer) {
    if (!Ready("VdInitializeRingBuffer")) return;
    GpuSystem& gpu = GpuSystem::Get();
    gpu.Cp().InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
    gpu.StartEngines();
}

// r3 = endereco do write-back do RPTR, r4 = log2 do bloco
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    if (!Ready("VdEnableRingBufferRPtrWriteBack")) return;
    GpuSystem::Get().Cp().EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// r3 = callback guest (source, user_data), r4 = user_data
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    if (!Ready("VdSetGraphicsInterruptCallback")) return;
    GpuSystem::Get().Interrupts().SetCallback(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    if (!Ready("VdSetSystemCommandBufferGpuIdentifierAddress")) return;
    GpuSystem::Get().gpuIdentifierAddress = ctx.r3.u32;
}

PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdShutdownEngines) { (void)ctx; }
PPC_FUNC(__imp__VdRetrainEDRAM) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// --- Modo de video e display ----------------------------------------------------------------

// r3 = ponteiro para X_VIDEO_MODE
PPC_FUNC(__imp__VdQueryVideoMode) {
    if (!Ready("VdQueryVideoMode")) return;
    WriteVideoMode(ctx.r3.u32);
}

PPC_FUNC(__imp__VdQueryVideoFlags) {
    // bit0 = widescreen, bit1 = largura >= 1024, bit2 = largura >= 1920
    ctx.r3.u64 = 1u | (kDisplayWidth >= 1024 ? 2u : 0u) | (kDisplayWidth >= 1920 ? 4u : 0u);
}

PPC_FUNC(__imp__VdSetDisplayMode) { ctx.r3.u64 = 0; }

// r3 = ponteiro para X_DISPLAY_INFO (0x58 bytes, big-endian)
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    if (!Ready("VdGetCurrentDisplayInformation")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    const GuestAddr p = ctx.r3.u32;
    Zero(p, 0x58);
    mem.StoreBe16(p + 0x00, kDisplayWidth);   // front_buffer_width
    mem.StoreBe16(p + 0x02, kDisplayHeight);  // front_buffer_height
    mem.StoreBe32(p + 0x10, kDisplayWidth);   // scaler_source_rect.x2
    mem.StoreBe32(p + 0x14, kDisplayHeight);  // scaler_source_rect.y2
    mem.StoreBe32(p + 0x18, kDisplayWidth);   // scaled_output_width
    mem.StoreBe32(p + 0x1C, kDisplayHeight);  // scaled_output_height
    mem.StoreBe32(p + 0x20, 1);               // vertical_filter_type
    mem.StoreBe32(p + 0x30, 1);               // horizontal_filter_type
    mem.StoreBe16(p + 0x40, 320);             // overscan left
    mem.StoreBe16(p + 0x42, 180);             // overscan top
    mem.StoreBe16(p + 0x44, 320);             // overscan right
    mem.StoreBe16(p + 0x46, 180);             // overscan bottom
    mem.StoreBe16(p + 0x48, kDisplayWidth);   // display_width
    mem.StoreBe16(p + 0x4A, kDisplayHeight);  // display_height
    mem.StoreBe32(p + 0x4C, FloatBits(kRefreshRate));
    mem.StoreBe16(p + 0x56, kDisplayWidth);   // actual_display_width
}

// r3 = ponteiro para o tipo (2 = BT.709), r4 = ponteiro para o expoente (float)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    if (!Ready("VdGetCurrentDisplayGamma")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    mem.StoreBe32(ctx.r3.u32, 2);
    mem.StoreBe32(ctx.r4.u32, FloatBits(2.22222233f));
}

// r3 = unk, r4 = ponteiro de saida: recebe um buffer de 64 bytes do guest. Retorna 1.
PPC_FUNC(__imp__VdPersistDisplay) {
    if (!Ready("VdPersistDisplay")) return;
    GpuSystem& gpu = GpuSystem::Get();
    if (ctx.r4.u32 != 0) {
        const GuestAddr buffer = gpu.Allocate(64);
        if (buffer != 0) Zero(buffer, 64);
        gpu.Memory().StoreBe32(ctx.r4.u32, buffer);
    }
    ctx.r3.u64 = 1;
}

// r3 = buffer de 0x94 bytes (zerado, primeiro dword = 0xBEEF0000), r4 = dword (0xBEEF0001)
PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    if (!Ready("VdGetSystemCommandBuffer")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    Zero(ctx.r3.u32, 0x94);
    mem.StoreBe32(ctx.r3.u32, 0xBEEF0000);
    mem.StoreBe32(ctx.r4.u32, 0xBEEF0001);
}

// Argumentos: r3..r10 = scaler_source_xy, scaler_source_wh, scaled_output_xy, scaled_output_wh,
// front_buffer_wh, vertical_filter_type, vertical_filter_params, horizontal_filter_type;
// pilha: horizontal_filter_params, unk9, dest_ptr, dest_count. Preenche dest com NOPs (tipo 2)
// e retorna dest_count.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    if (!Ready("VdInitializeScalerCommandBuffer")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    const GuestAddr dest = StackArg(ctx, 2);
    const uint32_t count = StackArg(ctx, 3);
    if (dest != 0 && Gpu::GuestMemory::Valid(dest, uint64_t(count) * 4)) {
        for (uint32_t i = 0; i < count; i++) mem.StoreBe32(dest + i * 4, 0x80000000u);
    }
    ctx.r3.u64 = count;
}

// --- Swap -----------------------------------------------------------------------------------

// r3 = buffer de 64 dwords reservado no ring pelo D3D, r4 = fetch constant (6 dwords) do frontbuffer,
// r8 = ponteiro do endereco do frontbuffer; pilha: [0] = ponteiro de largura, [1] = ponteiro de altura.
// Escreve no buffer: fetch constant (tipo 0) + pacote XE_SWAP + NOPs. O CP executa o swap.
PPC_FUNC(__imp__VdSwap) {
    if (!Ready("VdSwap")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();

    const GuestAddr buffer = ctx.r3.u32;
    const GuestAddr fetch = ctx.r4.u32;
    const GuestAddr frontbufferPtr = ctx.r8.u32;
    const GuestAddr widthPtr = StackArg(ctx, 0);
    const GuestAddr heightPtr = StackArg(ctx, 1);
    constexpr uint32_t kBufferDwords = 64;

    if (!Gpu::GuestMemory::Valid(buffer, kBufferDwords * 4) || !Gpu::GuestMemory::Valid(fetch, 24) ||
        frontbufferPtr == 0 || widthPtr == 0 || heightPtr == 0) {
        std::cerr << "[Vd] VdSwap com argumentos invalidos." << std::endl;
        return;
    }

    uint32_t fetchDwords[6];
    for (uint32_t i = 0; i < 6; i++) fetchDwords[i] = mem.LoadBe32(fetch + i * 4);

    // A fetch constant do D3D guarda o endereco virtual do frontbuffer em dword_1[31:12]. Como
    // MmGetPhysicalAddress e a identidade neste port, ele ja e o endereco que o CP usa.
    const uint32_t frontbuffer = (fetchDwords[1] >> 12) << 12;
    const uint32_t width = mem.LoadBe32(widthPtr);
    const uint32_t height = mem.LoadBe32(heightPtr);

    uint32_t offset = 0;
    auto emit = [&](uint32_t dword) { mem.StoreBe32(buffer + (offset++) * 4, dword); };

    emit(Gpu::Pm4::MakeType0(Gpu::Reg::kShaderConstantFetch00_0, 6));
    for (uint32_t dword : fetchDwords) emit(dword);

    emit(Gpu::Pm4::MakeType3(Gpu::Pm4::kXeSwap, 4));
    emit(Gpu::Pm4::kSwapSignature);
    emit(frontbuffer);
    emit(width);
    emit(height);

    while (offset < kBufferDwords) emit(Gpu::Pm4::MakeType2());
}
