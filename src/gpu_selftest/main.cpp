// Executavel de verificacao manual do caminho de GPU, sem o jogo nem o codigo recompilado:
//   janela Plume (Vulkan/D3D12) + command processor + imports Vd*, alimentados por um "D3D" simulado.
// Uso: gpu_selftest [frames] [--swap-only] [--screenshot arquivo.bmp]
//   padrao: cena de draw (Gpu::RunDrawSelfTest) com verificacao de pixels do frontbuffer;
//   --swap-only: so swaps pelo ring (Gpu::RunSelfTest, o teste do M1).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "graphics/renderer.h"
#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/selftest.h"
#include "gpu/selftest_scene.h"

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

    // BMP de 32 bits (BGRA, de baixo para cima).
    bool SaveBmp(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        const uint32_t imageSize = width * height * 4;
        const uint32_t fileSize = 54 + imageSize;
        uint8_t header[54] = { 'B', 'M' };
        auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
        put32(2, fileSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        header[26] = 1;
        header[28] = 32;
        put32(34, imageSize);
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<uint8_t> row(width * 4);
        for (uint32_t y = height; y-- > 0;) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                row[x * 4 + 0] = p[2];
                row[x * 4 + 1] = p[1];
                row[x * 4 + 2] = p[0];
                row[x * 4 + 3] = p[3];
            }
            out.write(reinterpret_cast<const char*>(row.data()), row.size());
        }
        return bool(out);
    }
}

int main(int argc, char** argv) {
    uint32_t frames = 300;
    bool swapOnly = false;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--swap-only") {
            swapOnly = true;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            frames = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
        }
    }

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
    const bool draws = !swapOnly && Graphics::NativeRenderer::AttachGuestMemory(base);
    if (!swapOnly && !draws) {
        std::cerr << "[GpuSelfTest] Sem renderer de draws (exige Vulkan): use --swap-only." << std::endl;
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

    std::cout << "[GpuSelfTest] " << frames << (draws ? " quadros com draws" : " swaps") << " pelo ring PM4"
              << " (ESC ou fechar a janela encerra)." << std::endl;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
    Gpu::SceneResult scene;
    std::thread feeder([&] {
        if (draws) {
            scene = Gpu::RunDrawSelfTest(frames, 16, &cancel);
            ok = scene.ok;
        } else {
            ok = Gpu::RunSelfTest(frames, 16, &cancel);
        }
        done = true;
    });

    while (Graphics::NativeRenderer::IsRunning() && !done.load()) {
        Graphics::NativeRenderer::PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel = true;
    feeder.join();

    // Para o CP antes de ler o frontbuffer: dai em diante so esta thread usa o device.
    Gpu::GpuSystem::Get().Cp().Stop();
    const Gpu::CpStats stats = Gpu::GpuSystem::Get().Cp().Stats();
    bool pixelsOk = true;
    if (draws && ok) {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
        // (o resolve de cor e o de profundidade tem as mesmas dimensoes)
        if (!Graphics::NativeRenderer::ReadbackResolved(scene.frontbuffer, width, height, rgba)) {
            std::cerr << "[GpuSelfTest] Frontbuffer resolvido nao encontrado." << std::endl;
            pixelsOk = false;
        } else {
            for (const Gpu::ScenePixel& p : scene.expected) {
                const uint8_t* got = &rgba[(size_t(p.y) * width + p.x) * 4];
                bool match = true;
                for (int c = 0; c < 4; ++c) match = match && std::abs(int(got[c]) - int(p.rgba[c])) <= 2;
                std::cout << "[GpuSelfTest] pixel (" << p.x << "," << p.y << "): " << int(got[0]) << " " << int(got[1]) << " "
                          << int(got[2]) << " " << int(got[3]) << " esperado " << int(p.rgba[0]) << " " << int(p.rgba[1])
                          << " " << int(p.rgba[2]) << " " << int(p.rgba[3]) << (match ? " OK" : " DIFERENTE") << std::endl;
                pixelsOk = pixelsOk && match;
            }
            if (!screenshot.empty()) {
                std::cout << "[GpuSelfTest] Screenshot: " << screenshot
                          << (SaveBmp(screenshot, width, height, rgba) ? "" : " (falhou)") << std::endl;
            }
        }
        std::vector<uint8_t> depth;
        if (!Graphics::NativeRenderer::ReadbackResolved(scene.depthTexture, width, height, depth)) {
            std::cerr << "[GpuSelfTest] Resolve de profundidade nao encontrado." << std::endl;
            pixelsOk = false;
        } else {
            for (const Gpu::SceneDepth& d : scene.expectedDepth) {
                float got;
                std::memcpy(&got, &depth[(size_t(d.y) * width + d.x) * 4], 4);
                const bool match = std::abs(got - d.depth) < 0.001f;
                std::cout << "[GpuSelfTest] profundidade (" << d.x << "," << d.y << "): " << got << " esperado " << d.depth
                          << (match ? " OK" : " DIFERENTE") << std::endl;
                pixelsOk = pixelsOk && match;
            }
        }
        std::cout << "[GpuSelfTest] Renderer: " << Graphics::NativeRenderer::XenosStatsText() << std::endl;
    }

    const bool passed = ok && pixelsOk && stats.malformed == 0;
    std::cout << "[GpuSelfTest] " << (passed ? "OK" : "FALHOU") << ": swaps=" << stats.swaps << " draws=" << stats.draws
              << " copias=" << stats.copies << " pacotes=" << stats.packets << " malformed=" << stats.malformed << std::endl;

    Gpu::GpuSystem::Get().Shutdown();
    Graphics::NativeRenderer::Shutdown();
    VirtualFree(base, 0, MEM_RELEASE);
    return passed ? 0 : 1;
}
