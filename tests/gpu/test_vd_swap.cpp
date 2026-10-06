#include "vd_rig.h"

// --- Ring buffer, swap e interrupcoes -------------------------------------------

TEST(vd_swap_emits_fetch_constant_and_xe_swap_packet) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    const GuestAddr fetch = Allocate(0x1000);
    const GuestAddr frontPtr = Allocate(0x1000);
    const GuestAddr widthPtr = frontPtr + 4;
    const GuestAddr heightPtr = frontPtr + 8;
    const GuestAddr front = 0x42345000;

    const uint32_t fetchDwords[6] = { 0x00000002, front | 0x6, 0x0059F4FF, 0x1111, 0x2222, 0x3333 };
    for (uint32_t i = 0; i < 6; i++) rig.mem.StoreBe32(fetch + i * 4, fetchDwords[i]);
    rig.mem.StoreBe32(frontPtr, front);
    rig.mem.StoreBe32(widthPtr, 1280);
    rig.mem.StoreBe32(heightPtr, 720);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54, widthPtr);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x5C, heightPtr);

    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = fetch;
    rig.ctx.r8.u64 = frontPtr;
    __imp__VdSwap(rig.ctx, rig.mem.Base());

    CHECK_EQ(rig.mem.LoadBe32(buffer + 0), Pm4::MakeType0(Reg::kShaderConstantFetch00_0, 6));
    for (uint32_t i = 0; i < 6; i++) CHECK_EQ(rig.mem.LoadBe32(buffer + 4 + i * 4), fetchDwords[i]);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 28), Pm4::MakeType3(Pm4::kXeSwap, 4));
    CHECK_EQ(rig.mem.LoadBe32(buffer + 32), Pm4::kSwapSignature);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 36), front);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 40), 1280);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 44), 720);
    for (uint32_t i = 12; i < 64; i++) CHECK_EQ(rig.mem.LoadBe32(buffer + i * 4), 0x80000000);
}

TEST(vd_swap_with_invalid_arguments_leaves_the_buffer_untouched) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = Allocate(0x1000);
    rig.ctx.r8.u64 = 0;  // ponteiro do frontbuffer nulo
    __imp__VdSwap(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(buffer), 0);
}

TEST(vd_set_graphics_interrupt_callback_registers_the_guest_function) {
    VdRig rig(true);
    rig.ctx.r3.u64 = 0x82005000;
    rig.ctx.r4.u64 = 0x1234;
    __imp__VdSetGraphicsInterruptCallback(rig.ctx, rig.mem.Base());

    GpuSystem::Get().Interrupts().Dispatch(1, 0);
    CHECK_EQ(rig.calls.size(), 1);
    CHECK_EQ(rig.calls[0].fn, 0x82005000);
    CHECK_EQ(rig.calls[0].a0, 1);
    CHECK_EQ(rig.calls[0].a1, 0x1234);
}

TEST(vd_initialize_ring_buffer_with_invalid_size_does_not_crash) {
    VdRig rig;
    rig.ctx.r3.u64 = Allocate(0x1000);
    rig.ctx.r4.u64 = 99;  // size_log2 absurdo
    __imp__VdInitializeRingBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.backend.swapCount.load(), 0);
}
