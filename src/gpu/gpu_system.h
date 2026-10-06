#pragma once

#include "gpu/command_processor.h"
#include "gpu/gpu_backend.h"
#include "gpu/guest_memory.h"
#include "gpu/interrupts.h"

#include <atomic>
#include <functional>
#include <memory>

namespace Gpu {
    struct GpuSystemConfig {
        uint8_t* base = nullptr;                          // base do espaco de 4 GB do guest
        IGpuBackend* backend = nullptr;                   // destino dos swaps
        std::function<GuestAddr(uint32_t)> allocate;      // aloca memoria do guest (VdPersistDisplay etc.)
        GuestCaller caller;                               // executa callbacks guest (interrupcoes)
        bool enableInterrupts = true;
    };

    // Estado global da GPU emulada: os imports Vd* sao PPC_FUNC livres, entao acessam isto via Get().
    class GpuSystem {
    public:
        static GpuSystem& Get();

        // Exige a pagina MMIO (Reg::kMmioBase, Reg::kMmioSize) commitada em `base`.
        bool Initialize(const GpuSystemConfig& config);
        void Shutdown();
        bool IsInitialized() const { return m_initialized.load(); }

        // Inicia a thread do CP e a de vblank (idempotente). Chamado por VdInitializeRingBuffer.
        void StartEngines();

        const GuestMemory& Memory() const { return m_memory; }
        CommandProcessor& Cp() { return *m_cp; }
        InterruptDispatcher& Interrupts() { return *m_interrupts; }
        GuestAddr Allocate(uint32_t size) const { return m_allocate ? m_allocate(size) : 0; }

        // VdSetSystemCommandBufferGpuIdentifierAddress: guardado, sem uso no M1 (o Xenia o ignora;
        // os fences chegam por EVENT_WRITE_SHD no stream de comandos).
        GuestAddr gpuIdentifierAddress = 0;

    private:
        GpuSystem() = default;

        std::atomic<bool> m_initialized{false};
        std::mutex m_engineMutex;
        bool m_started = false;
        GuestMemory m_memory;
        std::function<GuestAddr(uint32_t)> m_allocate;
        std::unique_ptr<InterruptDispatcher> m_interrupts;
        std::unique_ptr<CommandProcessor> m_cp;
    };
}
