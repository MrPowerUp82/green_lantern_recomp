#pragma once

#include "gpu/draw/primitive_processor.h"
#include "gpu/draw_call.h"

#include <array>
#include <cstdint>

// Estado de render de um draw, extraido dos registradores do Xenos (sem API grafica).
// As formulas de viewport, scissor e NDC seguem draw_util.cc do Xenia (sem escala de resolucao).
namespace Gpu::Draw {
    constexpr uint32_t kMaxColorTargets = 4;

    struct ColorTarget {
        bool enabled = false;
        uint32_t edramBase = 0;  // em tiles
        xenos::ColorRenderTargetFormat format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
        int32_t expBias = 0;
        uint32_t writeMask = 0;  // bits RGBA
        reg::RB_BLENDCONTROL blend{};
    };

    struct DepthTarget {
        bool enabled = false;
        uint32_t edramBase = 0;
        xenos::DepthRenderTargetFormat format = xenos::DepthRenderTargetFormat::kD24S8;
    };

    enum class CullMode { kNone, kFront, kBack };

    struct Viewport {
        float x = 0, y = 0, width = 0, height = 0, zMin = 0, zMax = 1;
    };

    struct Scissor {
        int32_t x = 0, y = 0;
        uint32_t width = 0, height = 0;
    };

    struct RenderState {
        // Superficie do EDRAM.
        uint32_t surfacePitch = 0;
        xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;
        ColorTarget color[kMaxColorTargets];
        DepthTarget depth;

        // Profundidade e stencil.
        bool depthTest = false;
        bool depthWrite = false;
        xenos::CompareFunction depthFunc = xenos::CompareFunction::kAlways;
        bool stencilTest = false;
        reg::RB_DEPTHCONTROL depthControl{};
        reg::RB_STENCILREFMASK stencilFront{};
        reg::RB_STENCILREFMASK stencilBack{};

        // Rasterizacao.
        CullMode cull = CullMode::kNone;
        bool frontFaceClockwise = false;
        float polyOffsetScale = 0.0f;   // em unidades de profundidade por pixel de inclinacao
        float polyOffsetConstant = 0.0f;

        Viewport viewport;
        Scissor scissor;

        // Constantes de sistema (draw_constants.h, Sys::*).
        std::array<uint32_t, 64> system{};
    };

    // `pixelShaderWritesDepth`: o PS exporta oDepth (o Xenia entao nao comprime a faixa de Z).
    RenderState ComputeRenderState(const DrawCall& draw, const HostPrimitive& primitive, bool pixelShaderWritesDepth);

    // Scissor final em pixels da superficie (janela do guest + scissor de tela + window offset).
    Scissor ComputeScissor(const RegisterFileView& regs);

    // Valor do clear de cor (RB_COLOR_CLEAR[_LO]) convertido para float no formato do RT.
    std::array<float, 4> UnpackColorClear(xenos::ColorRenderTargetFormat format, uint32_t clearHi, uint32_t clearLo);
    // Profundidade [0, 1] e stencil de RB_DEPTH_CLEAR.
    void UnpackDepthClear(xenos::DepthRenderTargetFormat format, uint32_t clear, float& depth, uint32_t& stencil);

    // Bytes por pixel do formato do RT no EDRAM (4 ou 8).
    bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format);
}
