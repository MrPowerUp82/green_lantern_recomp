#include <iostream>
#include <chrono>
#include <string>
#include "graphics/renderer.h"

int main(int argc, char** argv) {
    std::cout << "====================================================\n";
    std::cout << " Green Lantern Recomp - GPU Self-Test (M1)          \n";
    std::cout << "====================================================\n";

    int maxFrames = -1;
    int timeoutMs = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            maxFrames = std::stoi(argv[++i]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        }
    }

    Graphics::RenderConfig config;
    config.width = 1280;
    config.height = 720;
    config.backend = Graphics::Backend::Vulkan;
    config.allowBackendFallback = true;
    config.clearColor[0] = 0.0f;
    config.clearColor[1] = 0.35f;
    config.clearColor[2] = 0.15f; // Lantern green
    config.clearColor[3] = 1.0f;

    std::cout << "[gpu_selftest] Inicializando renderizador Vulkan...\n";
    if (!Graphics::NativeRenderer::Initialize(config)) {
        std::cerr << "[gpu_selftest] Falha ao inicializar o renderizador nativo.\n";
        return 1;
    }

    std::cout << "[gpu_selftest] Renderizador ativo: " << Graphics::NativeRenderer::GetBackendName() << "\n";
    std::cout << "[gpu_selftest] Pressione ESC ou feche a janela para sair.\n";

    auto startTime = std::chrono::steady_clock::now();
    int frameCount = 0;

    while (Graphics::NativeRenderer::IsRunning()) {
        Graphics::NativeRenderer::BeginFrame();
        Graphics::NativeRenderer::EndFrame();
        frameCount++;

        if (maxFrames > 0 && frameCount >= maxFrames) {
            std::cout << "[gpu_selftest] Limite de frames atingido (" << maxFrames << ").\n";
            break;
        }

        if (timeoutMs > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= timeoutMs) {
                std::cout << "[gpu_selftest] Timeout atingido (" << timeoutMs << " ms).\n";
                break;
            }
        }
    }

    std::cout << "[gpu_selftest] Finalizando renderizador...\n";
    Graphics::NativeRenderer::Shutdown();
    std::cout << "[gpu_selftest] Teste concluido com sucesso (" << frameCount << " frames renderizados).\n";

    return 0;
}
