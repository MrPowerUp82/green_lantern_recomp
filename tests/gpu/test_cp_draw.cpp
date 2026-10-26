#include "cp_rig.h"

using namespace Gpu;
using namespace TestSupport;

namespace {
    // VGT_DRAW_INITIATOR: prim_type | source_select << 6 | index_size << 11 | num_indices << 16
    constexpr uint32_t DrawInitiator(xenos::PrimitiveType prim, xenos::SourceSelect source, uint32_t count,
                                     bool index32 = false) {
        return uint32_t(prim) | (uint32_t(source) << 6) | (index32 ? (1u << 11) : 0u) | (count << 16);
    }
}

// --- IM_LOAD ----------------------------------------------------------------------------------

TEST(cp_im_load_immediate_sets_active_vertex_shader) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 5), 0 /* vertex */, 3, 0x11, 0x22, 0x33 });
    CHECK(rig.Submit());
    const ShaderBlob* vs = rig.cp.ActiveVertexShader();
    CHECK(vs != nullptr);
    if (vs) {
        CHECK(vs->type == xenos::ShaderType::kVertex);
        CHECK_EQ(vs->ucode.size(), 3);
        CHECK_EQ(vs->ucode[0], 0x11);
        CHECK_EQ(vs->ucode[2], 0x33);
    }
    CHECK(rig.cp.ActivePixelShader() == nullptr);
    CHECK_EQ(rig.cp.Stats().shaderLoads, 1);
}

TEST(cp_im_load_reads_big_endian_ucode_from_memory) {
    CpRig rig;
    const GuestAddr ucode = WriteGuestDwords({ 0xAABBCCDD, 0x01020304, 0x05060708 });
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoad, 2), ucode | 1 /* pixel */, 3 });
    CHECK(rig.Submit());
    const ShaderBlob* ps = rig.cp.ActivePixelShader();
    CHECK(ps != nullptr);
    if (ps) {
        CHECK(ps->type == xenos::ShaderType::kPixel);
        CHECK_EQ(ps->ucode[0], 0xAABBCCDD);
        CHECK_EQ(ps->ucode[1], 0x01020304);
    }
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_im_load_reuses_blob_for_identical_ucode) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 4), 1, 2, 7, 8 });
    CHECK(rig.Submit());
    const ShaderBlob* first = rig.cp.ActivePixelShader();
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 4), 1, 2, 9, 9 });
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 4), 1, 2, 7, 8 });
    CHECK(rig.Submit());
    CHECK(rig.cp.ActivePixelShader() == first);
}

TEST(cp_im_load_immediate_size_beyond_packet_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 3), 0, 5, 0x11 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
}

// --- DRAW_INDX ----------------------------------------------------------------------------------

TEST(cp_draw_indx_auto_index_reaches_backend) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 3), 0, 1, 0x1234 });
    rig.Emit({ Pm4::MakeType3(Pm4::kImLoadImmediate, 3), 1, 1, 0x5678 });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 2), 0,
               DrawInitiator(xenos::PrimitiveType::kTriangleList, xenos::SourceSelect::kAutoIndex, 6) });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.drawCount.load(), 1);
    const DrawCall& draw = rig.backend.lastDraw;
    CHECK(draw.primitive == xenos::PrimitiveType::kTriangleList);
    CHECK_EQ(draw.indexCount, 6);
    CHECK(!draw.indexed);
    CHECK(draw.vertexShader == rig.cp.ActiveVertexShader());
    CHECK(draw.pixelShader == rig.cp.ActivePixelShader());
    CHECK_EQ(rig.cp.Stats().draws, 1);
}

TEST(cp_draw_indx_dma_carries_index_buffer) {
    CpRig rig;
    // VGT_DMA_SIZE: num_words = 12, swap_mode = k8in16 (1) nos bits 30:31
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 4), 0,
               DrawInitiator(xenos::PrimitiveType::kTriangleStrip, xenos::SourceSelect::kDMA, 10),
               0x40123456, 12u | (1u << 30) });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.drawCount.load(), 1);
    const DrawCall& draw = rig.backend.lastDraw;
    CHECK(draw.indexed);
    CHECK_EQ(draw.indexBase, 0x40123456);
    CHECK(draw.indexFormat == xenos::IndexFormat::kInt16);
    CHECK(draw.indexEndian == xenos::Endian::k8in16);
    CHECK_EQ(draw.indexBufferBytes, 24);
    CHECK_EQ(rig.cp.RegisterValue(xe::gpu::XE_GPU_REG_VGT_DMA_BASE), 0x40123456);
}

TEST(cp_draw_indx_2_has_no_viz_token) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx2, 1),
               DrawInitiator(xenos::PrimitiveType::kRectangleList, xenos::SourceSelect::kAutoIndex, 3) });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.drawCount.load(), 1);
    CHECK(rig.backend.lastDraw.primitive == xenos::PrimitiveType::kRectangleList);
    CHECK_EQ(rig.backend.lastDraw.indexCount, 3);
}

TEST(cp_draw_in_copy_mode_issues_copy) {
    CpRig rig;
    // RB_MODECONTROL.edram_mode = kCopy (6)
    rig.Emit({ Pm4::MakeType0(xe::gpu::XE_GPU_REG_RB_MODECONTROL, 1), uint32_t(xenos::EdramMode::kCopy) });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 2), 0,
               DrawInitiator(xenos::PrimitiveType::kRectangleList, xenos::SourceSelect::kAutoIndex, 3) });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.copyCount.load(), 1);
    CHECK_EQ(rig.backend.drawCount.load(), 0);
    CHECK_EQ(rig.cp.Stats().copies, 1);
}

TEST(cp_predicated_packet_is_skipped_when_bins_do_not_match) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kSetBinSelectLo, 1), 0 });
    rig.Emit({ Pm4::MakeType3(Pm4::kSetBinSelectHi, 1), 0 });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 2, true), 0,
               DrawInitiator(xenos::PrimitiveType::kTriangleList, xenos::SourceSelect::kAutoIndex, 3) });
    rig.Emit({ Pm4::MakeType3(Pm4::kSetBinSelectLo, 1), 1 });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 2, true), 0,
               DrawInitiator(xenos::PrimitiveType::kTriangleList, xenos::SourceSelect::kAutoIndex, 3) });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.drawCount.load(), 1);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_event_write_zpd_writes_growing_little_endian_counts) {
    CpRig rig;
    const GuestAddr report = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(xe::gpu::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR, 1), report });
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteZpd, 1), 0x15 });
    CHECK(rig.Submit());
    uint32_t zpassA1, zpassB1;
    std::memcpy(&zpassA1, GuestBase() + report + 16, 4);
    std::memcpy(&zpassB1, GuestBase() + report + 20, 4);
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteZpd, 1), 0x15 });
    CHECK(rig.Submit());
    uint32_t zpassA2, zpassB2;
    std::memcpy(&zpassA2, GuestBase() + report + 16, 4);
    std::memcpy(&zpassB2, GuestBase() + report + 20, 4);
    CHECK(zpassA1 + zpassB1 > 0);
    CHECK(zpassA2 + zpassB2 > zpassA1 + zpassB1);
    CHECK_EQ(rig.cp.Stats().unhandled, 0);
}
