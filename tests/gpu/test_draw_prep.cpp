#include "test_support.h"

#include "gpu/draw/draw_state.h"
#include "gpu/draw/primitive_processor.h"
#include "gpu/draw/resolve_info.h"
#include "gpu/draw/texture_loader.h"
#include "gpu/shader/draw_constants.h"

#include "xenia/gpu/texture_address.h"

#include <array>
#include <atomic>
#include <bit>
#include <memory>

using namespace Gpu;
using namespace Gpu::Draw;
using namespace TestSupport;
namespace Sys = Gpu::Shader::Sys;

namespace {
    // Register file avulso para montar DrawCall sem o CP.
    struct Registers {
        std::unique_ptr<std::array<std::atomic<uint32_t>, Reg::kRegisterCount>> regs =
            std::make_unique<std::array<std::atomic<uint32_t>, Reg::kRegisterCount>>();
        void Set(uint32_t index, uint32_t value) { (*regs)[index] = value; }
        void SetFloat(uint32_t index, float value) { (*regs)[index] = std::bit_cast<uint32_t>(value); }
        RegisterFileView View() const { return RegisterFileView(regs->data()); }
    };

    DrawCall MakeDraw(const Registers& r, xenos::PrimitiveType prim, uint32_t count) {
        DrawCall draw;
        draw.regs = r.View();
        draw.primitive = prim;
        draw.indexCount = count;
        return draw;
    }

    float AsFloat(uint32_t v) { return std::bit_cast<float>(v); }
}

// --- Primitivas ------------------------------------------------------------------------------

TEST(primitive_triangle_list_auto_index_is_passthrough) {
    Registers r;
    const HostPrimitive p = ProcessPrimitive(MakeDraw(r, xenos::PrimitiveType::kTriangleList, 6), GuestMemory(GuestBase()));
    CHECK(p.ok);
    CHECK(!p.indexed);
    CHECK_EQ(p.vertexCount, 6);
    CHECK(p.topology == HostTopology::kTriangleList);
    CHECK(p.polygonal);
}

TEST(primitive_indexed_16bit_swaps_endian) {
    Registers r;
    const GuestAddr ib = Allocate(0x1000);
    const GuestMemory mem(GuestBase());
    // Indices 1, 2, 3 em big-endian (k8in16).
    const uint8_t bytes[] = { 0x00, 0x01, 0x00, 0x02, 0x00, 0x03 };
    std::memcpy(GuestBase() + ib, bytes, sizeof(bytes));
    DrawCall draw = MakeDraw(r, xenos::PrimitiveType::kTriangleList, 3);
    draw.indexed = true;
    draw.indexBase = ib;
    draw.indexFormat = xenos::IndexFormat::kInt16;
    draw.indexEndian = xenos::Endian::k8in16;
    draw.indexBufferBytes = 6;
    const HostPrimitive p = ProcessPrimitive(draw, mem);
    CHECK(p.ok);
    CHECK(p.indexed);
    CHECK_EQ(p.indices.size(), 3);
    CHECK_EQ(p.indices[0], 1);
    CHECK_EQ(p.indices[2], 3);
}

TEST(primitive_strip_reset_index_becomes_restart) {
    Registers r;
    r.Set(xe::gpu::XE_GPU_REG_PA_SU_SC_MODE_CNTL, 1u << 21);  // multi_prim_ib_ena
    r.Set(xe::gpu::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFF);
    const GuestAddr ib = Allocate(0x1000);
    const uint16_t indices[] = { 0, 1, 2, 0xFFFF, 3, 4, 5 };
    std::memcpy(GuestBase() + ib, indices, sizeof(indices));  // ja em little-endian: sem troca
    DrawCall draw = MakeDraw(r, xenos::PrimitiveType::kTriangleStrip, 7);
    draw.indexed = true;
    draw.indexBase = ib;
    draw.indexBufferBytes = sizeof(indices);
    const HostPrimitive p = ProcessPrimitive(draw, GuestMemory(GuestBase()));
    CHECK(p.ok);
    CHECK(p.topology == HostTopology::kTriangleStrip);
    CHECK_EQ(p.indices[3], kRestartIndex);
    CHECK_EQ(p.indices[4], 3);
}

TEST(primitive_quad_list_becomes_two_triangles_per_quad) {
    Registers r;
    const HostPrimitive p = ProcessPrimitive(MakeDraw(r, xenos::PrimitiveType::kQuadList, 8), GuestMemory(GuestBase()));
    CHECK(p.ok);
    CHECK(p.indexed);
    CHECK_EQ(p.indices.size(), 12);
    const uint32_t expected[] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    for (uint32_t i = 0; i < 12; ++i) CHECK_EQ(p.indices[i], expected[i]);
}

TEST(primitive_rectangle_list_draws_six_vertices_per_rect) {
    Registers r;
    const HostPrimitive p = ProcessPrimitive(MakeDraw(r, xenos::PrimitiveType::kRectangleList, 6), GuestMemory(GuestBase()));
    CHECK(p.ok);
    CHECK(p.rectangleList);
    CHECK_EQ(p.vertexCount, 12);
    CHECK_EQ(p.indices.size(), 6);
}

TEST(primitive_line_loop_closes_the_loop) {
    Registers r;
    const HostPrimitive p = ProcessPrimitive(MakeDraw(r, xenos::PrimitiveType::kLineLoop, 3), GuestMemory(GuestBase()));
    CHECK(p.ok);
    CHECK(p.topology == HostTopology::kLineList);
    CHECK_EQ(p.indices.size(), 6);
    CHECK_EQ(p.indices[5], 0);
}

// --- Estado de render -------------------------------------------------------------------------

TEST(render_state_d3d_viewport_maps_clip_space_to_host_ndc) {
    Registers r;
    // Viewport D3D 1280x720: xscale = 640, yscale = -360, offsets 640/360, z [0, 1].
    r.Set(xe::gpu::XE_GPU_REG_PA_CL_VTE_CNTL, 0x3F | (1u << 10));  // escala/offset xyz, w0_fmt
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_XSCALE, 640.0f);
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_XOFFSET, 640.0f);
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_YSCALE, -360.0f);
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_YOFFSET, 360.0f);
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_ZSCALE, 1.0f);
    r.SetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_ZOFFSET, 0.0f);
    r.Set(xe::gpu::XE_GPU_REG_PA_CL_CLIP_CNTL, 1u << 19);  // dx_clip_space_def
    r.Set(xe::gpu::XE_GPU_REG_PA_SU_VTX_CNTL, 1);  // centro do pixel em .5 (sem meio pixel)
    r.Set(xe::gpu::XE_GPU_REG_RB_SURFACE_INFO, 1280);
    r.Set(xe::gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 1280 | (720u << 16));
    r.Set(xe::gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 8192 | (8192u << 16));
    const HostPrimitive p = ProcessPrimitive(MakeDraw(r, xenos::PrimitiveType::kTriangleList, 3), GuestMemory(GuestBase()));
    const RenderState s = ComputeRenderState(MakeDraw(r, xenos::PrimitiveType::kTriangleList, 3), p, false);
    CHECK_EQ(uint32_t(s.viewport.x), 0);
    CHECK_EQ(uint32_t(s.viewport.width), 1280);
    CHECK_EQ(uint32_t(s.viewport.height), 720);
    CHECK(AsFloat(s.system[Sys::kNdcScale]) == 1.0f);
    CHECK(AsFloat(s.system[Sys::kNdcScale + 1]) == -1.0f);  // y para baixo no host
    CHECK(AsFloat(s.system[Sys::kNdcOffset]) == 0.0f);
    CHECK(s.viewport.zMin == 0.0f && s.viewport.zMax == 1.0f);
    CHECK_EQ(s.scissor.width, 1280);
    CHECK_EQ(s.scissor.height, 720);
    CHECK(s.system[Sys::kFlags] & Sys::kFlagWNotReciprocal);
}

TEST(render_state_reads_color_targets_and_depth) {
    Registers r;
    r.Set(xe::gpu::XE_GPU_REG_RB_MODECONTROL, uint32_t(xenos::EdramMode::kColorDepth));
    r.Set(xe::gpu::XE_GPU_REG_RB_COLOR_MASK, 0xF);
    r.Set(xe::gpu::XE_GPU_REG_RB_COLOR_INFO, 0x10 | (uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) << 16));
    r.Set(xe::gpu::XE_GPU_REG_RB_DEPTHCONTROL, 0x2 | 0x4 | (uint32_t(xenos::CompareFunction::kLessEqual) << 4));
    r.Set(xe::gpu::XE_GPU_REG_PA_SU_SC_MODE_CNTL, 0x2);  // cull_back
    const DrawCall draw = MakeDraw(r, xenos::PrimitiveType::kTriangleList, 3);
    const RenderState s = ComputeRenderState(draw, ProcessPrimitive(draw, GuestMemory(GuestBase())), false);
    CHECK(s.color[0].enabled);
    CHECK(!s.color[1].enabled);
    CHECK_EQ(s.color[0].edramBase, 0x10);
    CHECK(s.depthTest && s.depthWrite);
    CHECK(s.depthFunc == xenos::CompareFunction::kLessEqual);
    CHECK(s.cull == CullMode::kBack);
    CHECK_EQ(s.system[Sys::kGammaTargets], 1);
}

TEST(color_clear_unpacks_8888_and_depth_clear_unpacks_d24s8) {
    const auto c = UnpackColorClear(xenos::ColorRenderTargetFormat::k_8_8_8_8, 0xFF8000FFu, 0);
    CHECK(c[0] == 1.0f && c[1] == 0.0f && c[3] == 1.0f);
    CHECK(c[2] > 0.5f && c[2] < 0.51f);
    float depth;
    uint32_t stencil;
    UnpackDepthClear(xenos::DepthRenderTargetFormat::kD24S8, 0xFFFFFF00u | 0x7F, depth, stencil);
    CHECK(depth == 1.0f);
    CHECK_EQ(stencil, 0x7F);
}

// --- Resolve ----------------------------------------------------------------------------------

TEST(resolve_info_reads_rect_from_vertices_and_destination) {
    Registers r;
    const GuestAddr vb = Allocate(0x1000);
    const GuestMemory mem(GuestBase());
    const float verts[] = { 0.0f, 0.0f, 1280.0f, 0.0f, 1280.0f, 720.0f };
    for (int i = 0; i < 6; ++i) mem.StoreBe32(vb + i * 4, std::bit_cast<uint32_t>(verts[i]));
    // fetch constant 0: tipo vertex (3), endereco em dwords, endian k8in32, 6 dwords.
    r.Set(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 3u | vb);
    r.Set(xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1, 2u | (6u << 2));
    r.Set(xe::gpu::XE_GPU_REG_PA_SU_VTX_CNTL, 1);
    r.Set(xe::gpu::XE_GPU_REG_RB_SURFACE_INFO, 1280);
    r.Set(xe::gpu::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 1280 | (720u << 16));
    r.Set(xe::gpu::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 8192 | (8192u << 16));
    r.Set(xe::gpu::XE_GPU_REG_RB_COPY_CONTROL, 1u << 8);  // fonte RT0, clear de cor
    r.Set(xe::gpu::XE_GPU_REG_RB_COPY_DEST_BASE, 0x4F000000);
    r.Set(xe::gpu::XE_GPU_REG_RB_COPY_DEST_PITCH, 1280 | (720u << 16));
    r.Set(xe::gpu::XE_GPU_REG_RB_COPY_DEST_INFO, uint32_t(xenos::ColorFormat::k_8_8_8_8) << 7);
    const ResolveInfo info = ComputeResolveInfo(r.View(), mem);
    CHECK(info.valid);
    CHECK(info.copy);
    CHECK_EQ(info.x, 0);
    CHECK_EQ(info.width, 1280);
    CHECK_EQ(info.height, 720);
    CHECK_EQ(info.destBase, 0x4F000000);
    CHECK(info.clearColor);
    CHECK(!info.sourceIsDepth);
}

// --- Texturas -----------------------------------------------------------------------------------

namespace {
    // Fetch constant 2D: formato, dimensoes, pitch/32, tiled, endian, endereco base.
    xenos::xe_gpu_texture_fetch_t MakeTextureFetch(xenos::TextureFormat format, uint32_t width, uint32_t height,
                                                   bool tiled, xenos::Endian endian, GuestAddr base) {
        xenos::xe_gpu_texture_fetch_t f{};
        f.dword_0 = uint32_t(xenos::FetchConstantType::kTexture) | (((width + 31) / 32) << 22) | (tiled ? (1u << 31) : 0u);
        f.dword_1 = uint32_t(format) | (uint32_t(endian) << 6) | (base & 0xFFFFF000u);
        f.dword_2 = (width - 1) | ((height - 1) << 13);
        f.dword_3 = 0;
        f.dword_4 = 0;
        f.dword_5 = uint32_t(xenos::DataDimension::k2DOrStacked) << 9;
        return f;
    }
}

TEST(texture_linear_8888_with_8in32_endian) {
    const GuestAddr tex = Allocate(0x2000);
    const GuestMemory mem(GuestBase());
    // Linha de 32 texels (pitch minimo); texel (x, y) = 0xAABBGGRR com R = x, G = y.
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 32; ++x) mem.StoreBe32(tex + (y * 32 + x) * 4, 0xFF000000u | (y << 8) | x);
    }
    const auto fetch = MakeTextureFetch(xenos::TextureFormat::k_8_8_8_8, 2, 2, false, xenos::Endian::k8in32, tex);
    const TextureDescription desc = DescribeTexture(fetch);
    CHECK(desc.valid);
    const DecodedTexture t = DecodeTexture(desc, mem);
    CHECK(t.ok);
    CHECK(t.format == HostTextureFormat::kR8G8B8A8Unorm);
    CHECK_EQ(t.subresources.size(), 1);
    if (t.ok && !t.subresources.empty()) {
        const uint8_t* p = t.data.data() + t.subresources[0].offset;
        CHECK_EQ(p[4], 1);   // texel (1,0): R = 1
        CHECK_EQ(p[3], 0xFF);
        CHECK_EQ(p[t.subresources[0].rowBytes + 1], 1);  // texel (0,1): G = 1
    }
}

TEST(texture_tiled_2d_matches_xenia_tiled_addressing) {
    const GuestAddr tex = Allocate(0x10000);
    const GuestMemory mem(GuestBase());
    // 32x32 k_8_8_8_8 tiled, sem endian: escreve cada texel no endereco tiled com valor x | y << 8.
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const int32_t offset = xe::gpu::texture_address::Tiled2D(int32_t(x), int32_t(y), 32, 2);
            const uint32_t value = x | (y << 8);
            std::memcpy(GuestBase() + tex + offset, &value, 4);
        }
    }
    const auto fetch = MakeTextureFetch(xenos::TextureFormat::k_8_8_8_8, 32, 32, true, xenos::Endian::kNone, tex);
    const DecodedTexture t = DecodeTexture(DescribeTexture(fetch), mem);
    CHECK(t.ok);
    bool allMatch = t.ok;
    if (t.ok) {
        const uint8_t* p = t.data.data() + t.subresources[0].offset;
        for (uint32_t y = 0; y < 32 && allMatch; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                uint32_t v;
                std::memcpy(&v, p + y * t.subresources[0].rowBytes + x * 4, 4);
                if (v != (x | (y << 8))) {
                    allMatch = false;
                    break;
                }
            }
        }
    }
    CHECK(allMatch);
}

TEST(texture_565_expands_to_rgba8) {
    const GuestAddr tex = Allocate(0x2000);
    const GuestMemory mem(GuestBase());
    // Texel 0: vermelho total (x = 31 nos bits 0..4), sem endian.
    const uint16_t texel = 0x001F;
    std::memcpy(GuestBase() + tex, &texel, 2);
    const auto fetch = MakeTextureFetch(xenos::TextureFormat::k_5_6_5, 1, 1, false, xenos::Endian::kNone, tex);
    const DecodedTexture t = DecodeTexture(DescribeTexture(fetch), mem);
    CHECK(t.ok);
    if (t.ok) {
        const uint8_t* p = t.data.data() + t.subresources[0].offset;
        CHECK_EQ(p[0], 255);
        CHECK_EQ(p[1], 0);
        CHECK_EQ(p[3], 255);
    }
}

TEST(texture_dxt1_is_passed_through_as_bc1) {
    const GuestAddr tex = Allocate(0x2000);
    const GuestMemory mem(GuestBase());
    for (uint32_t i = 0; i < 64; ++i) GuestBase()[tex + i] = uint8_t(i);
    const auto fetch = MakeTextureFetch(xenos::TextureFormat::k_DXT1, 8, 8, false, xenos::Endian::kNone, tex);
    const DecodedTexture t = DecodeTexture(DescribeTexture(fetch), mem);
    CHECK(t.ok);
    CHECK(t.format == HostTextureFormat::kBC1);
    if (t.ok) {
        CHECK_EQ(t.subresources[0].rowBytes, 16);  // 2 blocos de 8 bytes
        CHECK_EQ(t.subresources[0].rowCount, 2);
        CHECK_EQ(t.data[t.subresources[0].offset + 9], 9);
    }
}

TEST(texture_memory_hash_changes_when_guest_writes) {
    const GuestAddr tex = Allocate(0x2000);
    const auto fetch = MakeTextureFetch(xenos::TextureFormat::k_8_8_8_8, 4, 4, false, xenos::Endian::kNone, tex);
    const TextureDescription desc = DescribeTexture(fetch);
    const GuestMemory mem(GuestBase());
    const uint64_t before = HashTextureMemory(desc, mem);
    GuestBase()[tex + 4] ^= 0x5A;
    CHECK(HashTextureMemory(desc, mem) != before);
}
