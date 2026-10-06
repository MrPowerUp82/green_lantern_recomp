// Executavel de verificacao manual do caminho de GPU, sem o jogo nem o codigo recompilado:
//   janela Plume (Vulkan/D3D12) + command processor + imports Vd*, alimentados por um "D3D" simulado
//   (Gpu::RunSelfTest). Uso: gpu_selftest [frames]
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <windows.h>

#include "graphics/renderer.h"
#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/selftest.h"

namespace {
    constexpr GuestAddr kHeapBase = 0x40000000;
    constexpr uint32_t kHeapSize = 0x04000000;  // 64 MB

    // Espaco de 4 GB do guest: so o heap e a pagina MMIO da GPU sao commitados.
    uint8_t* CreateGuestMemory() {
        uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE));
        if (!base) return nullptr;
        if (!VirtualAlloc(base + kHeapBase, kHeapSize, MEM_COMMIT, PAGE_READWRITE) ||
            !VirtualAlloc(base + Gpu::Reg::kMmioBase, Gpu::Reg::kMmioSize, MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(base, 0, MEM_RELEASE);
            return nullptr;
        }
        return base;
    }

    GuestAddr AllocateGuest(uint32_t size) {
        static std::atomic<GuestAddr> cursor{kHeapBase};
        return cursor.fetch_add((size + 0xFFFFu) & ~0xFFFFu);
    }
}

int main(int argc, char** argv) {
    const uint32_t frames = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 300;

    uint8_t* base = CreateGuestMemory();
    if (!base) {
        std::cerr << "[GpuSelfTest] Falha ao criar a memoria do guest." << std::endl;
        return 1;
    }

    Graphics::RenderConfig renderConfig;
    if (!Graphics::NativeRenderer::Initialize(renderConfig)) {
        std::cerr << "[GpuSelfTest] Falha ao inicializar o renderizador nativo." << std::endl;
        VirtualFree(base, 0, MEM_RELEASE);
        return 1;
    }

    Graphics::RendererBackend backend;
    Gpu::GpuSystemConfig config;
    config.base = base;
    config.backend = &backend;
    config.allocate = AllocateGuest;
    config.enableInterrupts = false;  // sem codigo guest para receber o callback
    if (!Gpu::GpuSystem::Get().Initialize(config)) {
        Graphics::NativeRenderer::Shutdown();
        VirtualFree(base, 0, MEM_RELEASE);
        return 1;
    }

    std::cout << "[GpuSelfTest] " << frames << " frames pelo ring PM4 (ESC ou fechar a janela encerra)." << std::endl;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
    std::thread feeder([&] {
        ok = Gpu::RunSelfTest(frames, 16, &cancel);
        done = true;
    });

    while (Graphics::NativeRenderer::IsRunning() && !done.load()) {
        Graphics::NativeRenderer::PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel = true;
    feeder.join();

    const Gpu::CpStats stats = Gpu::GpuSystem::Get().Cp().Stats();
    std::cout << "[GpuSelfTest] " << (ok ? "OK" : "FALHOU") << ": swaps=" << stats.swaps
              << " pacotes=" << stats.packets << " malformed=" << stats.malformed << std::endl;

    Gpu::GpuSystem::Get().Shutdown();
    Graphics::NativeRenderer::Shutdown();
    VirtualFree(base, 0, MEM_RELEASE);
    return (ok && stats.malformed == 0) ? 0 : 1;
}
