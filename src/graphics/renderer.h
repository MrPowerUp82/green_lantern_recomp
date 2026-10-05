#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace Graphics {
    enum class Backend {
        Direct3D12,
        Vulkan
    };

    struct RenderConfig {
        uint32_t width = 1280;
        uint32_t height = 720;
        bool vsync = true;
        bool fullscreen = false;
        Backend backend = Backend::Vulkan;
        // Se o backend pedido falhar ao inicializar, tenta o outro automaticamente.
        bool allowBackendFallback = true;
        // Cor de limpeza do framebuffer (RGBA).
        float clearColor[4] = { 0.0f, 0.18f, 0.05f, 1.0f };
    };

    class NativeRenderer {
    public:
        static bool Initialize(const RenderConfig& config);
        static void Shutdown();

        // Adquire a imagem da swapchain e inicia a gravacao do frame.
        static void BeginFrame();
        // Finaliza a gravacao, submete na fila e apresenta (vkQueuePresentKHR / Present).
        static void EndFrame();

        static bool IsRunning();
        static void PollEvents();

        static uint32_t GetWidth() { return s_config.width; }
        static uint32_t GetHeight() { return s_config.height; }
        static Backend GetActiveBackend() { return s_config.backend; }
        static const char* GetBackendName();

    private:
        static RenderConfig s_config;
        static bool s_initialized;
        static bool s_running;
    };
}
