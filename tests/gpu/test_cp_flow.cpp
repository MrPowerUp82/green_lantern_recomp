#include "cp_rig.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace Gpu;
using namespace TestSupport;

// --- Indirect buffers, WAIT_REG_MEM, swap, interrupcoes e thread -------------------------------

TEST(cp_indirect_buffer_executes_nested_packets) {
    CpRig rig;
    const GuestAddr ib = WriteGuestDwords({ Pm4::MakeType0(0x4800, 1), 0x77 });
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), ib, 2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x77);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_indirect_buffer_can_chain_another_indirect_buffer) {
    CpRig rig;
    const GuestAddr inner = WriteGuestDwords({ Pm4::MakeType0(0x4800, 1), 0x88 });
    const GuestAddr outer = WriteGuestDwords({ Pm4::MakeType3(Pm4::kIndirectBufferPfd, 2), inner, 2 });
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), outer, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x88);
}

TEST(cp_self_referencing_indirect_buffer_hits_depth_limit) {
    CpRig rig;
    const GuestAddr loop = Allocate(0x1000);
    rig.mem.StoreBe32(loop, Pm4::MakeType3(Pm4::kIndirectBuffer, 2));
    rig.mem.StoreBe32(loop + 4, loop);
    rig.mem.StoreBe32(loop + 8, 3);
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), loop, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);  // o CP sobrevive
}

TEST(cp_indirect_buffer_with_null_pointer_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), 0, 4 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
}

// --- WAIT_REG_MEM -----------------------------------------------------------------------------

TEST(cp_wait_reg_mem_memory_completes_when_value_arrives) {
    CpRig rig;
    const GuestAddr flag = Allocate(0x1000);
    // 0x13 = "igual" (3) + polling em memoria (0x10); o endereco carrega o modo 8in32
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x13, flag | 2, 1, 0xFFFFFFFF, 0 });
    rig.Emit({ Pm4::MakeType0(0x4800, 1), 3 });

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        rig.mem.StoreBe32(flag, 1);
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(rig.Submit());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    writer.join();

    CHECK(elapsed >= std::chrono::milliseconds(20));
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 3);  // o pacote depois do WAIT executou
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_wait_reg_mem_register_synchronizes_coherency_and_never_hangs) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(Reg::kCoherStatusHost, 1), 0x01000000 });  // marca pendente (bit 31)
    // espera COHER_STATUS_HOST == 0: so vale depois que o CP sincroniza
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x3, Reg::kCoherStatusHost, 0, 0xFFFFFFFF, 0 });
    // registrador que nunca vai satisfazer a condicao: ignorado, sem travar o CP
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x3, 0x0800, 5, 0xFFFFFFFF, 0 });
    rig.Emit({ Pm4::MakeType0(0x4800, 1), 4 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(Reg::kCoherStatusHost), 0);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 4);
}

TEST(cp_stop_aborts_a_wait_that_never_completes) {
    CpRig rig;
    const GuestAddr flag = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x13, flag | 2, 1, 0xFFFFFFFF, 0 });
    rig.Publish();
    rig.cp.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // o CP entra no WAIT
    rig.cp.Stop();                                               // deve retornar (join)
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

// --- Swap, interrupcoes e thread --------------------------------------------------------------

TEST(cp_xe_swap_calls_backend_with_frontbuffer_and_size) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), Pm4::kSwapSignature, 0x40100000, 1280, 720 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.swapCount.load(), 1);
    CHECK_EQ(rig.backend.lastFrontbuffer, 0x40100000);
    CHECK_EQ(rig.backend.lastWidth, 1280);
    CHECK_EQ(rig.backend.lastHeight, 720);
    CHECK_EQ(rig.cp.Stats().swaps, 1);
    CHECK_EQ(rig.cp.Counter(), 1);
}

TEST(cp_xe_swap_with_bad_signature_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), 0x12345678, 0x40100000, 1280, 720 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.swapCount.load(), 0);
    CHECK_EQ(rig.cp.Stats().malformed, 1);
}

TEST(cp_interrupt_dispatches_once_per_cpu_in_mask) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kInterrupt, 1), 0b101 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.sink.calls.size(), 2);
    CHECK_EQ(rig.sink.calls[0].source, 1);
    CHECK_EQ(rig.sink.calls[0].cpu, 0);
    CHECK_EQ(rig.sink.calls[1].cpu, 2);
}

TEST(cp_thread_consumes_ring_published_by_the_guest) {
    CpRig rig;
    rig.cp.Start();
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), Pm4::kSwapSignature, 0x40100000, 1280, 720 });
    rig.Publish();  // o D3D so escreve o WPTR; a thread do CP faz o polling

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.backend.swapCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rig.cp.Stop();
    CHECK_EQ(rig.backend.swapCount.load(), 1);
    CHECK_EQ(rig.backend.lastWidth, 1280);
}

TEST(cp_stop_interrupts_even_the_largest_wait_interval) {
    CpRig rig;
    const GuestAddr flag = Allocate(0x1000);
    rig.mem.StoreBe32(flag, 0);
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x13, flag | 2, 1, 0xFFFFFFFF, 0xFFFFFFFF });
    rig.Publish();
    rig.cp.Start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.cp.Stats().packets == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK_EQ(rig.cp.Stats().packets, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    rig.cp.Stop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}
