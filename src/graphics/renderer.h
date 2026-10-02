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
        Backend backend = Backend::Direct3D12;
    };

    class NativeRenderer {
    public:
        static bool Initialize(const RenderConfig& config);
        static void Shutdown();

        static void BeginFrame();
        static void EndFrame();

        static bool IsRunning();
        static void PollEvents();

        static uint32_t GetWidth() { return s_config.width; }
        static uint32_t GetHeight() { return s_config.height; }

    private:
        static RenderConfig s_config;
        static bool s_initialized;
        static bool s_running;
    };
}
