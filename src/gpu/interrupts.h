#pragma once

#include "gpu/command_processor.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace Gpu {
    // Chama a funcao guest `fn(arg0, arg1)` numa thread/contexto guest. Em producao e o
    // Kernel::CreateGuestCaller; nos testes e um fake que so registra a chamada.
    using GuestCaller = std::function<void(GuestAddr fn, uint32_t arg0, uint32_t arg1)>;

    // Despacha o callback registrado por VdSetGraphicsInterruptCallback.
    //   source 0 = vblank, source 1 = pacote PM4 INTERRUPT.
    class InterruptDispatcher final : public IInterruptSink {
    public:
        explicit InterruptDispatcher(GuestCaller caller);
        ~InterruptDispatcher() override;

        void SetCallback(GuestAddr fn, uint32_t userData);
        // Desligado, Dispatch nao chama o guest (flag --no-gpu-interrupts); o vblank continua contando.
        void SetEnabled(bool enabled) { m_enabled.store(enabled); }

        // cpu e informativo: o guest recebe (source, userData), como no Xbox 360.
        void Dispatch(uint32_t source, uint32_t cpu) override;

        // Thread de vblank: a cada 1/hz s chama onTick() e despacha source 0.
        void StartVblank(std::function<void()> onTick, uint32_t hz = 60);
        void StopVblank();

    private:
        GuestCaller m_caller;
        std::atomic<bool> m_enabled{true};

        std::mutex m_callbackMutex;
        GuestAddr m_callback = 0;
        uint32_t m_userData = 0;

        std::mutex m_callMutex;  // o contexto guest do callback nao e reentrante

        std::mutex m_stopMutex;
        std::condition_variable m_stopCv;
        bool m_stopRequested = false;
        std::thread m_vblankThread;
    };
}
