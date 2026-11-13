#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <cstdint>
#include <vector>

#include "gpu/gpu_backend.h"

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

    // Janela + device Plume (Vulkan ou D3D12).
    // Threads: Initialize/Shutdown/PollEvents/IsRunning rodam na thread principal (dona da janela);
    // PresentFrame roda na thread do command processor. Depois de Initialize, so a thread do CP
    // toca no device ate o Shutdown (que acontece depois do join do CP).
    class NativeRenderer {
    public:
        static bool Initialize(const RenderConfig& config);
        static void Shutdown();

        // Liga o renderer de draws do Xenos (so no backend Vulkan: os shaders traduzidos sao SPIR-V).
        // Chamar depois de Initialize e antes de iniciar o CP. false se o backend nao suporta draws.
        static bool AttachGuestMemory(uint8_t* guestBase, bool physicalAlias = false);
        static bool HasXenosRenderer();

        // Draws e resolves do Xenos (thread do CP).
        static bool IssueDraw(const Gpu::DrawCall& draw);
        static bool IssueCopy(const Gpu::RegisterFileView& regs);

        // Adquire a imagem da swapchain, apresenta o frontbuffer resolvido em `frontbufferAddr` (ou
        // limpa, se nao houver), submete e apresenta (vkQueuePresentKHR / Present).
        // Chamado pelo command processor a cada pacote XE_SWAP.
        static void PresentFrame(uint32_t frontbufferAddr = 0, uint32_t frontbufferWidth = 0, uint32_t frontbufferHeight = 0);

        // Copia a textura resolvida em `address` (RGBA8, ou R32F se veio de profundidade). Para o
        // selftest, com o CP parado.
        static bool ReadbackResolved(uint32_t address, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);
        // Estatisticas do renderer de draws (texto para log).
        static std::string XenosStatsText();
        // Marca a swapchain para recriacao no proximo PresentFrame. Thread-safe.
        static void NotifyResize();

        static bool IsRunning();
        static void PollEvents();

        static uint32_t GetWidth() { return s_config.width; }
        static uint32_t GetHeight() { return s_config.height; }
        static Backend GetActiveBackend() { return s_config.backend; }
        static const char* GetBackendName();

    private:
        static RenderConfig s_config;
        static bool s_initialized;
        static std::atomic<bool> s_running;
    };

    // Liga o command processor ao renderer: XE_SWAP -> PresentFrame.
    class RendererBackend final : public Gpu::IGpuBackend {
    public:
        void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) override;
        void OnResize(uint32_t width, uint32_t height) override;
        bool IssueDraw(const Gpu::DrawCall& draw) override;
        bool IssueCopy(const Gpu::RegisterFileView& regs) override;
    };
}
