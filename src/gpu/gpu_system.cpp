#include "gpu/gpu_system.h"

#include "gpu/gpu_registers.h"

#include <iostream>

namespace Gpu {
    namespace {
        struct MmioDefault {
            uint32_t reg;
            uint32_t value;
        };

        // Valores que o D3D le da pagina MMIO (mesmos que o Xenia devolve nas leituras).
        constexpr MmioDefault kMmioDefaults[] = {
            { Reg::kCpRbWptr, 0 },
            { Reg::kRbEdramTiming, 0x08100748 },
            { Reg::kRbBcControl, 0x0000200E },
            { Reg::kD1ModeVCounter, 0x000002D0 },
            { Reg::kInterruptStatus, 0x00000001 },       // vblank
            { Reg::kD1ModeViewportSize, 0x050002D0 },    // 1280x720
        };
    }

    GpuSystem& GpuSystem::Get() {
        static GpuSystem instance;
        return instance;
    }

    bool GpuSystem::Initialize(const GpuSystemConfig& config) {
        if (m_initialized.load()) Shutdown();
        if (!config.base || !config.backend) {
            std::cerr << "[GPU] GpuSystem::Initialize sem base ou backend." << std::endl;
            return false;
        }

        m_memory = GuestMemory(config.base);
        m_allocate = config.allocate;
        gpuIdentifierAddress = 0;

        for (const MmioDefault& d : kMmioDefaults) {
            m_memory.StoreBe32(Reg::MmioAddress(d.reg), d.value);
        }

        m_interrupts = std::make_unique<InterruptDispatcher>(config.caller);
        m_interrupts->SetEnabled(config.enableInterrupts);
        m_cp = std::make_unique<CommandProcessor>(config.base, config.backend, m_interrupts.get());

        m_started = false;
        m_initialized = true;
        return true;
    }

    void GpuSystem::StartEngines() {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        if (!m_initialized || m_started) return;
        m_started = true;
        CommandProcessor* cp = m_cp.get();
        m_cp->Start();
        m_interrupts->StartVblank([cp] { cp->IncrementCounter(); });
    }

    void GpuSystem::Shutdown() {
        // Imports que ja passaram pela verificacao Ready ainda podem ter uma referencia ao CP.
        // Mantemos os objetos parados vivos ate a proxima inicializacao, feita sem guest ativo.
        {
            std::lock_guard<std::mutex> lock(m_engineMutex);
            if (!m_initialized.exchange(false)) return;
            m_started = false;
        }
        // Nunca manter o mutex de engines durante join: um callback guest pode chamar Vd*.
        m_interrupts->StopVblank();
        m_cp->Stop();
    }
}
