#include "gpu/interrupts.h"
#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace Gpu;

namespace {
    struct FakeGuest {
        struct Call { GuestAddr fn; uint32_t arg0; uint32_t arg1; };

        GuestCaller Caller() {
            return [this](GuestAddr fn, uint32_t a0, uint32_t a1) {
                std::lock_guard<std::mutex> lock(mutex);
                calls.push_back({ fn, a0, a1 });
            };
        }
        size_t Count() {
            std::lock_guard<std::mutex> lock(mutex);
            return calls.size();
        }

        std::mutex mutex;
        std::vector<Call> calls;
    };
}

TEST(dispatcher_calls_guest_with_source_and_user_data) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 0xBEEF);
    dispatcher.Dispatch(1, 3);

    CHECK_EQ(guest.Count(), 1);
    CHECK_EQ(guest.calls[0].fn, 0x82001000);
    CHECK_EQ(guest.calls[0].arg0, 1);
    CHECK_EQ(guest.calls[0].arg1, 0xBEEF);
}

TEST(dispatcher_ignores_interrupts_until_a_callback_is_registered) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.Dispatch(0, 2);
    CHECK_EQ(guest.Count(), 0);
}

TEST(dispatcher_does_not_call_the_guest_when_disabled) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 1);
    dispatcher.SetEnabled(false);
    dispatcher.Dispatch(0, 2);
    CHECK_EQ(guest.Count(), 0);
}

TEST(vblank_thread_ticks_and_dispatches_source_zero) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 7);

    std::atomic<uint32_t> ticks{0};
    dispatcher.StartVblank([&] { ticks.fetch_add(1); }, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    dispatcher.StopVblank();

    CHECK(ticks.load() >= 5);
    CHECK_EQ(guest.Count(), ticks.load());
    for (const FakeGuest::Call& call : guest.calls) CHECK_EQ(call.arg0, 0);
}

TEST(vblank_ticks_even_without_a_registered_callback) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());

    std::atomic<uint32_t> ticks{0};
    dispatcher.StartVblank([&] { ticks.fetch_add(1); }, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dispatcher.StopVblank();

    CHECK(ticks.load() >= 3);
    CHECK_EQ(guest.Count(), 0);
}

TEST(vblank_stop_returns_promptly) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.StartVblank([] {}, 1);  // periodo de 1 s

    const auto start = std::chrono::steady_clock::now();
    dispatcher.StopVblank();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(500));
}
