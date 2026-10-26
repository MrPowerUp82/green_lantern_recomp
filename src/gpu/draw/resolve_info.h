#pragma once

#include "gpu/draw_call.h"
#include "gpu/guest_memory.h"

#include <cstdint>

namespace Gpu::Draw {
    // Um resolve (draw com RB_MODECONTROL == kCopy): copia um retangulo do EDRAM para uma textura
    // na memoria do guest e/ou limpa o render target. Segue GetResolveInfo do Xenia.
    struct ResolveInfo {
        bool valid = false;
        // Retangulo na superficie do EDRAM, em pixels, alinhado a 8.
        uint32_t x = 0, y = 0, width = 0, height = 0;
        uint32_t surfacePitch = 0;
        xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;

        // Origem: RT de cor 0..3, ou profundidade.
        bool sourceIsDepth = false;
        uint32_t sourceIndex = 0;
        uint32_t sourceEdramBase = 0;
        xenos::ColorRenderTargetFormat sourceColorFormat = xenos::ColorRenderTargetFormat::k_8_8_8_8;
        xenos::DepthRenderTargetFormat sourceDepthFormat = xenos::DepthRenderTargetFormat::kD24S8;

        // Destino (copy_command != kNull e pitch != 0).
        bool copy = false;
        GuestAddr destBase = 0;
        uint32_t destPitch = 0;   // em pixels
        uint32_t destHeight = 0;  // em pixels
        xenos::TextureFormat destFormat = xenos::TextureFormat::k_8_8_8_8;
        xenos::Endian128 destEndian = xenos::Endian128::kNone;
        int32_t destExpBias = 0;
        bool destSwapRedBlue = false;

        // Clears depois da copia.
        bool clearColor = false;
        bool clearDepth = false;
        uint32_t colorClear = 0, colorClearLo = 0, depthClear = 0;
    };

    ResolveInfo ComputeResolveInfo(const RegisterFileView& regs, const GuestMemory& memory);
}
