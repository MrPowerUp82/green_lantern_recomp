#include "cp_rig.h"

using namespace Gpu;
using namespace TestSupport;

namespace {
    constexpr uint32_t kType2 = 0x80000000;
}

// --- Decodificacao de pacotes, ring e write-back ---------------------------------------------

TEST(cp_idle_when_wptr_equals_rptr) {
    CpRig rig;
    CHECK(!rig.Submit());
    CHECK_EQ(rig.cp.ReadIndex(), 0);
}

TEST(cp_type0_writes_consecutive_registers) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 3), 0x11, 0x22, 0x33 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x11);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0x22);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0x33);
    CHECK_EQ(rig.cp.ReadIndex(), 4);
}

TEST(cp_type0_one_reg_writes_same_register) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 3, true), 1, 2, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 3);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0);
}

TEST(cp_wraps_around_ring_end) {
    CpRig rig(4);  // ring de 32 dwords
    for (int i = 0; i < 30; i++) rig.Emit({ kType2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.ReadIndex(), 30);

    rig.Emit({ Pm4::MakeType0(0x4800, 3), 0xA, 0xB, 0xC });  // ocupa os indices 30, 31, 0, 1
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0xA);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0xB);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0xC);
    CHECK_EQ(rig.cp.ReadIndex(), 2);
}

TEST(cp_type2_and_zero_headers_are_skipped) {
    CpRig rig;
    rig.Emit({ kType2, 0, kType2, Pm4::MakeType0(0x4800, 1), 7 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 7);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_writes_back_read_pointer_big_endian) {
    CpRig rig;
    const GuestAddr writeback = Allocate(0x1000);
    rig.cp.EnableReadPointerWriteBack(writeback, 6);
    rig.Emit({ kType2, kType2, kType2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(writeback), 3);
}

TEST(cp_type1_packet_is_malformed_and_dropped_until_wptr) {
    CpRig rig;
    rig.Emit({ 1u << 30, 0, 0, Pm4::MakeType0(0x4800, 1), 9 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0);  // descartado junto com o resto do lote
    CHECK_EQ(rig.cp.ReadIndex(), 5);

    rig.Emit({ Pm4::MakeType0(0x4800, 1), 5 });  // o CP continua vivo
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 5);
}

TEST(cp_type0_overflowing_the_batch_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 8), 1, 2 });  // declara 8 dwords, so ha 2
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0);
}

TEST(cp_unknown_opcode_is_skipped_without_losing_sync) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(0x7E, 3), 1, 2, 3, Pm4::MakeType0(0x4800, 1), 0x55 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().unhandled, 1);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x55);
}

// --- Opcodes de registrador e memoria ---------------------------------------------------------
