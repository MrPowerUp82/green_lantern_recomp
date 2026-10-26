#include "gpu/draw/resolve_info.h"

#include "gpu/draw/draw_state.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Gpu::Draw {
    namespace {
        constexpr int32_t kResolveAlignment = 8;  // xenos::kResolveAlignmentPixels

        // Arredondamento para ponto fixo 16.8 como o rasterizador do D3D11 (FloatToD3D11Fixed16p8).
        int32_t ToFixed16p8(float v) {
            if (!(v >= -32768.0f)) return -32768 * 256;
            if (v >= 32768.0f) return 32767 * 256 + 255;
            return int32_t(std::lround(v * 256.0f));
        }
    }

    ResolveInfo ComputeResolveInfo(const RegisterFileView& regs, const GuestMemory& memory) {
        ResolveInfo info;
        const auto copyControl = regs.Get<reg::RB_COPY_CONTROL>();
        const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
        info.surfacePitch = surface.surface_pitch;
        info.msaa = surface.msaa_samples;

        // O D3D desenha um retangulo (rect list de 3 vertices float2) na fetch constant 0.
        const xenos::xe_gpu_vertex_fetch_t fetch = regs.VertexFetch(0);
        if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6) return info;
        const GuestAddr vertices = fetch.address << 2;
        if (!memory.IsReadable(vertices, 24)) return info;
        const float halfPixel =
            regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
        int32_t v[6];
        for (int i = 0; i < 6; ++i) {
            uint32_t raw;
            std::memcpy(&raw, memory.Base() + vertices + i * 4, 4);
            v[i] = ToFixed16p8(std::bit_cast<float>(xenos::GpuSwap(raw, fetch.endian)) + halfPixel);
        }
        int32_t x0 = (std::min({ v[0], v[2], v[4] }) + 127) >> 8;
        int32_t y0 = (std::min({ v[1], v[3], v[5] }) + 127) >> 8;
        int32_t x1 = (std::max({ v[0], v[2], v[4] }) + 127) >> 8;
        int32_t y1 = (std::max({ v[1], v[3], v[5] }) + 127) >> 8;
        if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
            const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            x0 += offset.window_x_offset;
            x1 += offset.window_x_offset;
            y0 += offset.window_y_offset;
            y1 += offset.window_y_offset;
        }
        const Scissor scissor = ComputeScissor(regs);
        const int32_t right = scissor.x + int32_t(scissor.width), bottom = scissor.y + int32_t(scissor.height);
        x0 = std::clamp(x0, scissor.x, right);
        x1 = std::clamp(x1, scissor.x, right);
        y0 = std::clamp(y0, scissor.y, bottom);
        y1 = std::clamp(y1, scissor.y, bottom);
        x0 &= ~(kResolveAlignment - 1);
        y0 &= ~(kResolveAlignment - 1);
        x1 = (x1 + kResolveAlignment - 1) & ~(kResolveAlignment - 1);
        y1 = (y1 + kResolveAlignment - 1) & ~(kResolveAlignment - 1);
        const int32_t pitchAligned = int32_t(surface.surface_pitch) & ~(kResolveAlignment - 1);
        x0 = std::min(x0, pitchAligned);
        x1 = std::min(x1, pitchAligned);
        if (x0 < x1 && y0 < y1) {
            info.x = uint32_t(x0);
            info.y = uint32_t(y0);
            info.width = uint32_t(x1 - x0);
            info.height = uint32_t(y1 - y0);
        }

        info.sourceIsDepth = copyControl.copy_src_select >= 4;
        info.sourceIndex = std::min<uint32_t>(copyControl.copy_src_select, 3);
        if (info.sourceIsDepth) {
            const auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
            info.sourceEdramBase = depthInfo.depth_base;
            info.sourceDepthFormat = depthInfo.depth_format;
        } else {
            const auto colorInfo = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[info.sourceIndex]);
            info.sourceEdramBase = colorInfo.color_base;
            info.sourceColorFormat = colorInfo.color_format;
        }

        const auto destInfo = regs.Get<reg::RB_COPY_DEST_INFO>();
        const auto destPitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
        info.destBase = regs[xe::gpu::XE_GPU_REG_RB_COPY_DEST_BASE];
        info.destPitch = destPitch.copy_dest_pitch;
        info.destHeight = destPitch.copy_dest_height;
        if (destInfo.copy_dest_array) {
            const uint32_t slice = regs[xe::gpu::XE_GPU_REG_RB_COPY_SURFACE_SLICE];
            if (slice && info.destPitch) info.destHeight = slice / info.destPitch;
        }
        info.destFormat = info.sourceIsDepth
                              ? (info.sourceDepthFormat == xenos::DepthRenderTargetFormat::kD24S8
                                     ? xenos::TextureFormat::k_24_8
                                     : xenos::TextureFormat::k_24_8_FLOAT)
                              : xenos::TextureFormat(destInfo.copy_dest_format);
        info.destEndian = destInfo.copy_dest_endian;
        info.destExpBias = destInfo.copy_dest_exp_bias;
        info.destSwapRedBlue = destInfo.copy_dest_swap != 0;
        info.copy = copyControl.copy_command != xenos::CopyCommand::kNull && info.destBase != 0 && info.destPitch != 0 &&
                    info.width != 0;

        info.clearColor = copyControl.color_clear_enable != 0;
        info.clearDepth = copyControl.depth_clear_enable != 0;
        info.colorClear = regs[xe::gpu::XE_GPU_REG_RB_COLOR_CLEAR];
        info.colorClearLo = regs[xe::gpu::XE_GPU_REG_RB_COLOR_CLEAR_LO];
        info.depthClear = regs[xe::gpu::XE_GPU_REG_RB_DEPTH_CLEAR];
        info.valid = true;
        return info;
    }
}
