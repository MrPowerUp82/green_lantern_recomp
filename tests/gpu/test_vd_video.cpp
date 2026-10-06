#include "vd_rig.h"

// --- Modo de video, display e imports triviais ---------------------------------------------

TEST(vd_query_video_mode_writes_big_endian_struct) {
    VdRig rig;
    const GuestAddr p = Allocate(0x1000);
    rig.ctx.r3.u64 = p;
    __imp__VdQueryVideoMode(rig.ctx, rig.mem.Base());

    CHECK_EQ(rig.mem.LoadBe32(p + 0x00), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x04), 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x08), 0);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x0C), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x10), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x14), 0x42700000);  // 60.0f
    CHECK_EQ(rig.mem.LoadBe32(p + 0x18), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x1C), 0x4A);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x20), 1);
}

TEST(vd_query_video_flags_reports_widescreen_hd) {
    VdRig rig;
    __imp__VdQueryVideoFlags(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 3);
}

TEST(vd_get_current_display_information_fills_scaler_and_display_fields) {
    VdRig rig;
    const GuestAddr p = Allocate(0x1000);
    rig.ctx.r3.u64 = p;
    __imp__VdGetCurrentDisplayInformation(rig.ctx, rig.mem.Base());

    const uint8_t* b = rig.mem.Base() + p;
    CHECK_EQ((b[0x00] << 8) | b[0x01], 1280);
    CHECK_EQ((b[0x02] << 8) | b[0x03], 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x10), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x14), 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x18), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x1C), 720);
    CHECK_EQ((b[0x40] << 8) | b[0x41], 320);
    CHECK_EQ((b[0x43] << 0) | (b[0x42] << 8), 180);
    CHECK_EQ((b[0x48] << 8) | b[0x49], 1280);
    CHECK_EQ((b[0x4A] << 8) | b[0x4B], 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x4C), 0x42700000);
    CHECK_EQ((b[0x56] << 8) | b[0x57], 1280);
}

TEST(vd_get_current_display_gamma_returns_bt709) {
    VdRig rig;
    const GuestAddr type = Allocate(0x1000);
    rig.ctx.r3.u64 = type;
    rig.ctx.r4.u64 = type + 4;
    __imp__VdGetCurrentDisplayGamma(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(type), 2);
    CHECK_EQ(rig.mem.LoadBe32(type + 4), 0x400E38E4);  // 2.22222233f
}

TEST(vd_trivial_exports_return_expected_values) {
    VdRig rig;
    uint8_t* base = rig.mem.Base();

    __imp__VdInitializeEngines(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 1);
    __imp__VdIsHSIOTrainingSucceeded(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 1);
    __imp__VdSetDisplayMode(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdRetrainEDRAM(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdRetrainEDRAMWorker(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdEnableDisableClockGating(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdCallGraphicsNotificationRoutines(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdShutdownEngines(rig.ctx, base);  // sem efeito observavel; nao pode quebrar
}

TEST(vd_persist_display_allocates_and_returns_its_pointer) {
    VdRig rig;
    const GuestAddr out = Allocate(0x1000);
    rig.ctx.r3.u64 = 0;
    rig.ctx.r4.u64 = out;
    __imp__VdPersistDisplay(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 1);
    CHECK(rig.mem.LoadBe32(out) != 0);
}

TEST(vd_get_system_command_buffer_writes_markers) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    const GuestAddr other = Allocate(0x1000);
    rig.mem.StoreBe32(buffer + 8, 0xFFFFFFFF);
    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = other;
    __imp__VdGetSystemCommandBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(buffer), 0xBEEF0000);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 8), 0);  // zerado
    CHECK_EQ(rig.mem.LoadBe32(other), 0xBEEF0001);
}

TEST(vd_initialize_scaler_command_buffer_fills_with_nops) {
    VdRig rig;
    const GuestAddr dest = Allocate(0x1000);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54 + 2 * 8, dest);  // dest_ptr
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54 + 3 * 8, 4);     // dest_count
    __imp__VdInitializeScalerCommandBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 4);
    CHECK_EQ(rig.mem.LoadBe32(dest), 0x80000000);
    CHECK_EQ(rig.mem.LoadBe32(dest + 12), 0x80000000);
    CHECK_EQ(rig.mem.LoadBe32(dest + 16), 0);
}

TEST(vd_set_system_command_buffer_gpu_identifier_address_is_stored) {
    VdRig rig;
    rig.ctx.r3.u64 = 0x40123450;
    __imp__VdSetSystemCommandBufferGpuIdentifierAddress(rig.ctx, rig.mem.Base());
    CHECK_EQ(GpuSystem::Get().gpuIdentifierAddress, 0x40123450);
}
