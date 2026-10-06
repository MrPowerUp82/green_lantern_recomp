#include "gpu/guest_memory.h"
#include "gpu/pm4.h"
#include "test_harness.h"

using namespace Gpu;

TEST(pm4_type0_header_roundtrip) {
    const uint32_t raw = Pm4::MakeType0(0x4800, 6);
    const Pm4::Header h = Pm4::DecodeHeader(raw);
    CHECK_EQ(h.type, 0);
    CHECK_EQ(h.count, 6);
    CHECK_EQ(h.baseIndex, 0x4800);
    CHECK(!h.oneReg);
}

TEST(pm4_type0_one_reg_flag) {
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType0(0x0A31, 3, true));
    CHECK_EQ(h.type, 0);
    CHECK_EQ(h.count, 3);
    CHECK_EQ(h.baseIndex, 0x0A31);
    CHECK(h.oneReg);
}

TEST(pm4_type2_header) {
    CHECK_EQ(Pm4::MakeType2(), 0x80000000);
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType2());
    CHECK_EQ(h.type, 2);
    CHECK_EQ(h.count, 0);
}

TEST(pm4_type3_header_roundtrip) {
    const uint32_t raw = Pm4::MakeType3(Pm4::kXeSwap, 4);
    CHECK_EQ(raw, 0xC0036400);
    const Pm4::Header h = Pm4::DecodeHeader(raw);
    CHECK_EQ(h.type, 3);
    CHECK_EQ(h.count, 4);
    CHECK_EQ(h.opcode, Pm4::kXeSwap);
    CHECK(!h.predicate);
}

TEST(pm4_type3_predicate_and_max_count) {
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType3(Pm4::kNop, 0x4000, true));
    CHECK_EQ(h.count, 0x4000);
    CHECK_EQ(h.opcode, Pm4::kNop);
    CHECK(h.predicate);
}

TEST(pm4_swap_signature_is_swap_fourcc) {
    CHECK_EQ(Pm4::kSwapSignature, (uint32_t('S') << 24) | (uint32_t('W') << 16) | (uint32_t('A') << 8) | uint32_t('P'));
}

TEST(gpu_swap_modes) {
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::None), 0x11223344);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k8in16), 0x22114433);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k8in32), 0x44332211);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k16in32), 0x33441122);
}
