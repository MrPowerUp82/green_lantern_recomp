#include "cp_rig.h"

using namespace Gpu;
using namespace TestSupport;

// --- Opcodes de registrador e memoria ---------------------------------------------------------

TEST(cp_set_constant_maps_each_type_to_its_register_base) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 3), (0u << 16) | 4, 0x1111, 0x2222 });  // ALU
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (1u << 16) | 2, 0x3333 });          // FETCH
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (2u << 16) | 0, 0x4444 });          // BOOL
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (3u << 16) | 1, 0x5555 });          // LOOP
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (4u << 16) | 1, 0x6666 });          // REGISTERS
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4004), 0x1111);
    CHECK_EQ(rig.cp.RegisterValue(0x4005), 0x2222);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0x3333);
    CHECK_EQ(rig.cp.RegisterValue(0x4900), 0x4444);
    CHECK_EQ(rig.cp.RegisterValue(0x4909), 0x5555);
    CHECK_EQ(rig.cp.RegisterValue(0x2001), 0x6666);
}

TEST(cp_set_constant2_and_set_shader_constants_use_absolute_index) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant2, 3), 0x4810, 0xAAAA, 0xBBBB });
    rig.Emit({ Pm4::MakeType3(Pm4::kSetShaderConstants, 2), 0x4000, 0xCCCC });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4810), 0xAAAA);
    CHECK_EQ(rig.cp.RegisterValue(0x4811), 0xBBBB);
    CHECK_EQ(rig.cp.RegisterValue(0x4000), 0xCCCC);
}

TEST(cp_load_alu_constant_reads_guest_memory) {
    CpRig rig;
    const GuestAddr source = WriteGuestDwords({ 0xDEAD0001, 0xDEAD0002 });
    rig.Emit({ Pm4::MakeType3(Pm4::kLoadAluConstant, 3), source, (0u << 16) | 2, 2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4002), 0xDEAD0001);
    CHECK_EQ(rig.cp.RegisterValue(0x4003), 0xDEAD0002);
}

TEST(cp_reg_rmw_with_immediate_masks) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0xFF00 });
    rig.Emit({ Pm4::MakeType3(Pm4::kRegRmw, 3), 0x0800, 0x0FF0, 0x000F });  // (0xFF00 & 0x0FF0) | 0xF
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x0800), 0x0F0F);
}

TEST(cp_reg_rmw_with_register_operands) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0xABCD });
    rig.Emit({ Pm4::MakeType0(0x0801, 1), 0x00FF });
    rig.Emit({ Pm4::MakeType0(0x0802, 1), 0x1000 });
    // bit 31: operando AND vem do registrador 0x0801; bit 30: operando OR vem do registrador 0x0802
    rig.Emit({ Pm4::MakeType3(Pm4::kRegRmw, 3), 0xC0000000 | 0x0800, 0x0801, 0x0802 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x0800), (0xABCD & 0x00FF) | 0x1000);
}

TEST(cp_reg_to_mem_swaps_by_address_bits) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0x11223344 });
    rig.Emit({ Pm4::MakeType3(Pm4::kRegToMem, 2), 0x0800, target | 2 });  // 8in32
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(target), 0x11223344);
}

TEST(cp_mem_write_writes_consecutive_dwords) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kMemWrite, 3), target | 2, 0xAABBCCDD, 0x01020304 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(target), 0xAABBCCDD);
    CHECK_EQ(rig.mem.LoadBe32(target + 4), 0x01020304);
}

TEST(cp_cond_write_to_memory_only_when_condition_matches) {
    CpRig rig;
    const GuestAddr hit = Allocate(0x1000);
    const GuestAddr miss = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0x1234 });
    // 0x103 = "igual" (3) + escrita em memoria (0x100); poll no registrador 0x0800
    rig.Emit({ Pm4::MakeType3(Pm4::kCondWrite, 6), 0x103, 0x0800, 0x1234, 0xFFFF, hit | 2, 0xCAFE });
    rig.Emit({ Pm4::MakeType3(Pm4::kCondWrite, 6), 0x103, 0x0800, 0x9999, 0xFFFF, miss | 2, 0xCAFE });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(hit), 0xCAFE);
    CHECK_EQ(rig.mem.LoadBe32(miss), 0);
}

TEST(cp_event_write_sets_initiator_register) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWrite, 1), 0x4000003F | 0x80 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(Reg::kVgtEventInitiator), 0x3F);
}

TEST(cp_event_write_shd_writes_value_then_counter) {
    CpRig rig;
    const GuestAddr fence = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteShd, 3), 0x00000005, fence | 2, 0xFEED });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(fence), 0xFEED);
    CHECK_EQ(rig.cp.RegisterValue(Reg::kVgtEventInitiator), 5);

    for (int i = 0; i < 7; i++) rig.cp.IncrementCounter();
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteShd, 3), 0x80000005, fence | 2, 0xFEED });  // bit 31: grava o contador
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(fence), 7);
}

TEST(cp_event_write_ext_writes_big_endian_extents) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteExt, 2), 0x1, target | 1 });
    CHECK(rig.Submit());
    const uint8_t* b = GuestBase() + target;
    CHECK_EQ((b[0] << 8) | b[1], 0);
    CHECK_EQ((b[2] << 8) | b[3], 1024);
    CHECK_EQ((b[6] << 8) | b[7], 1024);
    CHECK_EQ((b[10] << 8) | b[11], 1);
}

TEST(cp_scratch_registers_mirror_to_memory_when_enabled) {
    CpRig rig;
    const GuestAddr scratch = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(Reg::kScratchAddr, 1), scratch });
    rig.Emit({ Pm4::MakeType0(Reg::kScratchUmsk, 1), 0x2 });  // so o slot 1
    rig.Emit({ Pm4::MakeType0(Reg::kScratchReg0, 2), 5, 9 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(scratch), 0);
    CHECK_EQ(rig.mem.LoadBe32(scratch + 4), 9);
}

TEST(cp_draw_opcodes_are_counted_not_executed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 3), 1, 2, 3 });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx2, 2), 4, 5 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().draws, 2);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

// --- Indirect buffers -------------------------------------------------------------------------
