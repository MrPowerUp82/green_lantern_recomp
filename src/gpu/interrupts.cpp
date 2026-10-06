#include "gpu/interrupts.h"

#include <chrono>

namespace Gpu {
    InterruptDispatcher::InterruptDispatcher(GuestCaller caller) : m_caller(std::move(caller)) {}

    InterruptDispatcher::~InterruptDispatcher() { StopVblank(); }

    void InterruptDispatcher::SetCallback(GuestAddr fn, uint32_t userData) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = fn;
        m_userData = userData;
    }

    void InterruptDispatcher::Dispatch(uint32_t source, uint32_t /*cpu*/) {
        if (!m_enabled.load() || !m_caller) return;

        GuestAddr fn;
        uint32_t userData;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            fn = m_callback;
            userData = m_userData;
        }
        if (fn == 0) return;  // so depois de registrado

        std::lock_guard<std::mutex> lock(m_callMutex);
        m_caller(fn, source, userData);
    }

    void InterruptDispatcher::StartVblank(std::function<void()> onTick, uint32_t hz) {
        if (m_vblankThread.joinable() || hz == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopRequested = false;
        }
        const auto period = std::chrono::nanoseconds(1000000000ull / hz);
        m_vblankThread = std::thread([this, onTick = std::move(onTick), period] {
            auto next = std::chrono::steady_clock::now();
            while (true) {
                next += period;
                {
                    std::unique_lock<std::mutex> lock(m_stopMutex);
                    if (m_stopCv.wait_until(lock, next, [this] { return m_stopRequested; })) return;
                }
                if (onTick) onTick();
                Dispatch(0, 2);
            }
        });
    }

    void InterruptDispatcher::StopVblank() {
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopRequested = true;
        }
        m_stopCv.notify_all();
        if (m_vblankThread.joinable()) m_vblankThread.join();
    }
}
