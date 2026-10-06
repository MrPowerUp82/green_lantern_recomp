#pragma once

#include <atomic>
#include <cstdint>

namespace Gpu {
    // Fronteira entre o command processor e o renderer. O CP nao conhece Plume.
    class IGpuBackend {
    public:
        virtual ~IGpuBackend() = default;

        // Chamado na thread do CP quando um pacote XE_SWAP e executado.
        virtual void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) = 0;

        // Chamado de qualquer thread (ex.: WM_SIZE). Deve ser thread-safe e so marcar a mudanca.
        virtual void OnResize(uint32_t width, uint32_t height) = 0;
    };

    // Backend sem GPU: registra as chamadas. Usado nos testes e no selftest sem janela.
    class NullBackend final : public IGpuBackend {
    public:
        void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) override {
            lastFrontbuffer = frontbufferAddr;
            lastWidth = width;
            lastHeight = height;
            swapCount.fetch_add(1, std::memory_order_release);
        }
        void OnResize(uint32_t, uint32_t) override { resizeCount.fetch_add(1, std::memory_order_release); }

        std::atomic<uint32_t> swapCount{0};
        std::atomic<uint32_t> resizeCount{0};
        // Validos depois de observar swapCount > 0 (acquire).
        uint32_t lastFrontbuffer = 0;
        uint32_t lastWidth = 0;
        uint32_t lastHeight = 0;
    };
}
