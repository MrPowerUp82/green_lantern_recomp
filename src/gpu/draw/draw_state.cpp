#include "gpu/draw/draw_state.h"

#include "gpu/shader/draw_constants.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Gpu::Draw {
    namespace {
        using xe::gpu::Register;
        namespace Sys = Gpu::Shader::Sys;

        // Maior viewport aceito pelo host (maxViewportDimensions do Vulkan e >= 4096; usamos o
        // limite de textura 2D do Xenos, que cobre qualquer superficie do EDRAM).
        constexpr uint32_t kMaxViewport = 8192;

        uint32_t FloatBits(float f) { return std::bit_cast<uint32_t>(f); }

        float Half(uint16_t h) {
            // float16 IEEE (o mesmo do Xenos para limpar RTs FLOAT).
            const uint32_t sign = uint32_t(h >> 15) << 31;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mant = h & 0x3FF;
            uint32_t bits;
            if (exp == 0) {
                if (mant == 0) {
                    bits = sign;
                } else {
                    exp = 127 - 15 + 1;
                    while (!(mant & 0x400)) {
                        mant <<= 1;
                        --exp;
                    }
                    bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
                }
            } else if (exp == 31) {
                bits = sign | 0x7F800000u | (mant << 13);
            } else {
                bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
            }
            return std::bit_cast<float>(bits);
        }
    }

    bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format) {
        switch (format) {
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
            return true;
        default:
            return false;
        }
    }

    Scissor ComputeScissor(const RegisterFileView& regs) {
        const auto windowTl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
        const auto windowBr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
        const auto windowOffset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        const auto screenTl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
        const auto screenBr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();

        int32_t tlX = int32_t(windowTl.tl_x), tlY = int32_t(windowTl.tl_y);
        int32_t brX = int32_t(windowBr.br_x), brY = int32_t(windowBr.br_y);
        if (!windowTl.window_offset_disable) {
            tlX += windowOffset.window_x_offset;
            tlY += windowOffset.window_y_offset;
            brX += windowOffset.window_x_offset;
            brY += windowOffset.window_y_offset;
        }
        tlX = std::max(tlX, int32_t(screenTl.tl_x));
        tlY = std::max(tlY, int32_t(screenTl.tl_y));
        brX = std::min(brX, int32_t(screenBr.br_x));
        brY = std::min(brY, int32_t(screenBr.br_y));
        tlX = std::max(tlX, 0);
        tlY = std::max(tlY, 0);
        Scissor s;
        s.x = tlX;
        s.y = tlY;
        s.width = brX > tlX ? uint32_t(brX - tlX) : 0;
        s.height = brY > tlY ? uint32_t(brY - tlY) : 0;
        return s;
    }

    std::array<float, 4> UnpackColorClear(xenos::ColorRenderTargetFormat format, uint32_t clearHi, uint32_t clearLo) {
        std::array<float, 4> c{};
        // Formatos de 64 bpp: RB_COLOR_CLEAR_LO e a metade baixa do valor empacotado.
        const uint64_t value = IsColorFormat64bpp(format) ? (uint64_t(clearHi) << 32) | clearLo : clearHi;
        switch (format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
            for (int i = 0; i < 4; ++i) c[i] = float((value >> (i * 8)) & 0xFF) / 255.0f;
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
            for (int i = 0; i < 3; ++i) c[i] = float((value >> (i * 10)) & 0x3FF) / 1023.0f;
            c[3] = float((value >> 30) & 3) / 3.0f;
            break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            for (int i = 0; i < 3; ++i) c[i] = xenos::Float7e3To32(uint32_t(value >> (i * 10)) & 0x3FF);
            c[3] = float((value >> 30) & 3) / 3.0f;
            break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16: {
            // Ponto fixo com sinal na faixa -32..32.
            const int n = format == xenos::ColorRenderTargetFormat::k_16_16 ? 2 : 4;
            for (int i = 0; i < n; ++i) c[i] = float(int16_t(value >> (i * 16))) * (32.0f / 32767.0f);
            break;
        }
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            const int n = format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT ? 2 : 4;
            for (int i = 0; i < n; ++i) c[i] = Half(uint16_t(value >> (i * 16)));
            break;
        }
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
            c[0] = std::bit_cast<float>(uint32_t(value));
            break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
            c[0] = std::bit_cast<float>(uint32_t(value));
            c[1] = std::bit_cast<float>(uint32_t(value >> 32));
            break;
        }
        return c;
    }

    void UnpackDepthClear(xenos::DepthRenderTargetFormat format, uint32_t clear, float& depth, uint32_t& stencil) {
        const uint32_t d24 = (clear >> 8) & 0xFFFFFF;
        depth = format == xenos::DepthRenderTargetFormat::kD24S8 ? xenos::UNorm24To32(d24)
                                                                 : std::min(1.0f, xenos::Float20e4To32(d24));
        stencil = clear & 0xFF;
    }

    RenderState ComputeRenderState(const DrawCall& draw, const HostPrimitive& primitive, bool pixelShaderWritesDepth) {
        const RegisterFileView& regs = draw.regs;
        RenderState s;

        // --- Superficie e render targets.
        const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
        const auto mode = regs.Get<reg::RB_MODECONTROL>();
        s.surfacePitch = surface.surface_pitch;
        s.msaa = surface.msaa_samples;
        const bool colorEnabled = mode.edram_mode == xenos::EdramMode::kColorDepth;
        const uint32_t colorMask = regs[xe::gpu::XE_GPU_REG_RB_COLOR_MASK];
        for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
            const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            ColorTarget& rt = s.color[i];
            rt.writeMask = (colorMask >> (i * 4)) & 0xF;
            rt.enabled = colorEnabled && rt.writeMask != 0;
            rt.edramBase = info.color_base;
            rt.format = info.color_format;
            rt.expBias = info.color_exp_bias;
            rt.blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
        }

        const auto depthControl = regs.Get<reg::RB_DEPTHCONTROL>();
        const auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
        s.depthControl = depthControl;
        s.depthTest = depthControl.z_enable != 0;
        s.depthWrite = depthControl.z_enable && depthControl.z_write_enable;
        s.depthFunc = depthControl.zfunc;
        s.stencilTest = depthControl.stencil_enable != 0;
        s.stencilFront = regs.Get<reg::RB_STENCILREFMASK>();
        s.stencilBack = regs.Get<reg::RB_STENCILREFMASK>(xe::gpu::XE_GPU_REG_RB_STENCILREFMASK_BF);
        s.depth.enabled = s.depthTest || s.stencilTest;
        s.depth.edramBase = depthInfo.depth_base;
        s.depth.format = depthInfo.depth_format;

        // --- Rasterizacao: culling so para triangulos (rect lists nunca sao descartados).
        const auto modeCntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        if (primitive.polygonal && !primitive.rectangleList) {
            if (modeCntl.cull_front && modeCntl.cull_back) {
                s.cull = CullMode::kNone;  // descarta tudo: o renderer pula o draw via scissor vazio
                s.scissor = Scissor{};
            } else if (modeCntl.cull_front) {
                s.cull = CullMode::kFront;
            } else if (modeCntl.cull_back) {
                s.cull = CullMode::kBack;
            }
        }
        // face = 0: frente anti-horaria (no espaco de tela do Xenos, Y para baixo como no host).
        s.frontFaceClockwise = modeCntl.face != 0;
        if (primitive.polygonal) {
            float scale = 0.0f, offset = 0.0f;
            if (modeCntl.poly_offset_front_enable) {
                scale = regs.GetFloat(xe::gpu::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
                offset = regs.GetFloat(xe::gpu::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
            } else if (modeCntl.poly_offset_back_enable) {
                scale = regs.GetFloat(xe::gpu::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
                offset = regs.GetFloat(xe::gpu::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
            }
            // Xenos: escala em 1/16 de pixel; offset em unidades de profundidade de 24 bits.
            s.polyOffsetScale = scale * (1.0f / 16.0f);
            s.polyOffsetConstant = offset * float(1 << 24);
        }

        // --- Viewport e NDC (GetHostViewportInfo do Xenia, sem escala de resolucao).
        const auto clipCntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
        const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
        const auto vtxCntl = regs.Get<reg::PA_SU_VTX_CNTL>();
        const float scaleXY[2] = {
            vte.vport_x_scale_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
            vte.vport_y_scale_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f,
        };
        const float scaleZ = vte.vport_z_scale_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
        const float offsetBaseXY[2] = {
            vte.vport_x_offset_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
            vte.vport_y_offset_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f,
        };
        const float offsetZ = vte.vport_z_offset_ena ? regs.GetFloat(xe::gpu::XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
        float offsetAddXY[2] = {};
        if (modeCntl.vtx_window_offset_enable) {
            const auto windowOffset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            offsetAddXY[0] += float(windowOffset.window_x_offset);
            offsetAddXY[1] += float(windowOffset.window_y_offset);
        }
        if (vtxCntl.pix_center == xenos::PixelCenter::kD3DZero) {
            offsetAddXY[0] += 0.5f;
            offsetAddXY[1] += 0.5f;
        }

        float ndcScale[3], ndcOffset[3];
        float vpOffset[2], vpExtent[2];
        float zMin, zMax;
        if (clipCntl.clip_disable) {
            for (int i = 0; i < 2; ++i) {
                const float extent = float(kMaxViewport);
                vpOffset[i] = 0.0f;
                vpExtent[i] = extent;
                const float pixelsToNdc = 2.0f / extent;
                ndcScale[i] = scaleXY[i] * pixelsToNdc;
                ndcOffset[i] = (offsetBaseXY[i] - extent * 0.5f + offsetAddXY[i]) * pixelsToNdc;
            }
            zMin = 0.0f;
            zMax = 1.0f;
            ndcScale[2] = scaleZ;
            ndcOffset[2] = offsetZ;
        } else {
            for (int i = 0; i < 2; ++i) {
                const float offsetAxis = offsetBaseXY[i] + offsetAddXY[i];
                const float scaleAbs = std::abs(scaleXY[i]);
                const float axisMax = float(kMaxViewport);
                const int32_t axis1 = int32_t(std::floor(std::clamp(offsetAxis + scaleAbs, 0.0f, axisMax)));
                const int32_t axis0 =
                    int32_t(std::floor(std::clamp(offsetAxis - scaleAbs, float(axis1) - axisMax, axisMax)));
                const uint32_t extent = axis1 > 0 ? uint32_t(axis1 - axis0) : 0;
                vpOffset[i] = float(axis0);
                vpExtent[i] = float(extent);
                if (extent) {
                    const float inv = 1.0f / float(extent);
                    ndcScale[i] = scaleXY[i] * 2.0f * inv;
                    ndcOffset[i] = (offsetAxis - (float(axis0) + float(extent) * 0.5f)) * 2.0f * inv;
                } else {
                    ndcScale[i] = 1.0f;
                    ndcOffset[i] = 0.0f;
                }
            }
            float hostClipOffsetZ, hostClipScaleZ;
            if (clipCntl.dx_clip_space_def) {
                hostClipOffsetZ = offsetZ;
                hostClipScaleZ = scaleZ;
                ndcScale[2] = 1.0f;
                ndcOffset[2] = 0.0f;
            } else {
                hostClipOffsetZ = offsetZ - scaleZ;
                hostClipScaleZ = scaleZ * 2.0f;
                ndcScale[2] = 0.5f;
                ndcOffset[2] = 0.5f;
            }
            if (pixelShaderWritesDepth) {
                zMin = 0.0f;
                zMax = 1.0f;
            } else {
                zMin = std::clamp(hostClipOffsetZ, 0.0f, 1.0f);
                zMax = std::clamp(hostClipOffsetZ + hostClipScaleZ, 0.0f, 1.0f);
                if (zMin > zMax) {
                    std::swap(zMin, zMax);
                    ndcScale[2] = -ndcScale[2];
                    ndcOffset[2] = 1.0f - ndcOffset[2];
                }
            }
        }
        s.viewport = { vpOffset[0], vpOffset[1], vpExtent[0], vpExtent[1], zMin, zMax };
        if (!(primitive.polygonal && modeCntl.cull_front && modeCntl.cull_back)) s.scissor = ComputeScissor(regs);
        if (s.surfacePitch) {
            // O scissor nunca passa da superficie (o RT do host tem a largura do pitch).
            const int32_t right = std::min(int32_t(s.scissor.x + s.scissor.width), int32_t(s.surfacePitch));
            s.scissor.width = right > s.scissor.x ? uint32_t(right - s.scissor.x) : 0;
        }

        // --- Constantes de sistema.
        auto& sys = s.system;
        uint32_t flags = 0;
        if (vte.vtx_xy_fmt) flags |= Sys::kFlagXYDividedByW;
        if (vte.vtx_z_fmt) flags |= Sys::kFlagZDividedByW;
        if (vte.vtx_w0_fmt) flags |= Sys::kFlagWNotReciprocal;
        if (primitive.polygonal) flags |= Sys::kFlagPrimitivePolygonal;
        if (primitive.lines) flags |= Sys::kFlagPrimitiveLine;
        if (primitive.points) flags |= Sys::kFlagPrimitivePoint;
        if (primitive.rectangleList) flags |= Sys::kFlagIndicesFromRing;
        const auto colorControl = regs.Get<reg::RB_COLORCONTROL>();
        if (colorControl.alpha_test_enable && colorControl.alpha_func != xenos::CompareFunction::kAlways) {
            flags |= Sys::kFlagAlphaTest;
        }
        sys[Sys::kFlags] = flags;
        sys[Sys::kVertexIndexOffset] = regs[xe::gpu::XE_GPU_REG_VGT_INDX_OFFSET];
        sys[Sys::kVertexIndexMin] = regs[xe::gpu::XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
        sys[Sys::kVertexIndexMax] = regs[xe::gpu::XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
        if (sys[Sys::kVertexIndexMax] == 0) sys[Sys::kVertexIndexMax] = 0xFFFFFF;
        for (int i = 0; i < 3; ++i) {
            sys[Sys::kNdcScale + i] = FloatBits(ndcScale[i]);
            sys[Sys::kNdcOffset + i] = FloatBits(ndcOffset[i]);
        }
        const auto pointSize = regs.Get<reg::PA_SU_POINT_SIZE>();
        const auto pointMinMax = regs.Get<reg::PA_SU_POINT_MINMAX>();
        // Meia largura em ponto fixo 12.4 -> diametro em pixels.
        sys[Sys::kPointSize] = FloatBits(float(pointSize.width) * (2.0f / 16.0f));
        sys[Sys::kPointSizeMinMax] = FloatBits(float(pointMinMax.min_size) * (2.0f / 16.0f));
        sys[Sys::kPointSizeMinMax + 1] = FloatBits(std::max(1.0f, float(pointMinMax.max_size) * (2.0f / 16.0f)));
        sys[Sys::kAlphaRef] = FloatBits(regs.GetFloat(xe::gpu::XE_GPU_REG_RB_ALPHA_REF));
        sys[Sys::kAlphaFunc] = uint32_t(colorControl.alpha_func);

        const auto windowOffset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        sys[Sys::kParamGenWindowOffset] = FloatBits(float(windowOffset.window_x_offset));
        sys[Sys::kParamGenWindowOffset + 1] = FloatBits(float(windowOffset.window_y_offset));
        const auto programCntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
        const auto contextMisc = regs.Get<reg::SQ_CONTEXT_MISC>();
        sys[Sys::kParamGenRegister] = programCntl.param_gen ? contextMisc.param_gen_pos : 0xFFFFFFFFu;

        uint32_t gammaTargets = 0;
        for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
            sys[Sys::kColorExpBias + i] = FloatBits(std::ldexp(1.0f, s.color[i].expBias));
            if (s.color[i].format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) gammaTargets |= 1u << i;
        }
        sys[Sys::kGammaTargets] = gammaTargets;
        return s;
    }
}
