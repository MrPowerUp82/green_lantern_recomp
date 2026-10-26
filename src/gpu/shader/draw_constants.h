#pragma once

#include <cstdint>

// Bloco de constantes de cada draw, gravado pelo renderer no buffer de upload (SSBO) e lido pelos
// shaders traduzidos. O push constant `cb` aponta para o inicio do bloco (em dwords, alinhado a 4).
// Os offsets abaixo sao em dwords a partir do inicio do bloco; o tradutor os usa no GLSL gerado.
namespace Gpu::Shader::DrawBlock {
    // 512 vec4 de float constants do register file (VS usa 0..255, PS usa 256..511).
    constexpr uint32_t kFloatConstants = 0;
    constexpr uint32_t kFloatConstantCount = 512;
    constexpr uint32_t kPixelFloatConstantBase = 256;
    // 256 bool constants (8 dwords) e 32 loop constants, como no register file.
    constexpr uint32_t kBoolConstants = kFloatConstants + kFloatConstantCount * 4;  // 2048
    constexpr uint32_t kLoopConstants = kBoolConstants + 8;                           // 2056
    // 32 fetch constants de 6 dwords (texturas) == 96 de 2 dwords (vertices).
    constexpr uint32_t kFetchConstants = kLoopConstants + 32;                         // 2088
    // Para cada vertex fetch constant: delta (int32) somado ao endereco em dwords do guest para
    // achar o dword correspondente no buffer de upload. So os usados pelo shader sao validos.
    constexpr uint32_t kVertexRemap = kFetchConstants + 192;                          // 2280
    // Constantes de sistema (ver Sys::*).
    constexpr uint32_t kSystem = kVertexRemap + 96;                                   // 2376
    constexpr uint32_t kSystemSize = 64;
    constexpr uint32_t kSize = kSystem + kSystemSize;                                 // 2440

    static_assert(kSize % 4 == 0, "o bloco precisa manter o alinhamento de vec4");
}

namespace Gpu::Shader::Sys {
    // Dwords dentro da area de sistema (DrawBlock::kSystem + indice).
    constexpr uint32_t kFlags = 0;
    constexpr uint32_t kVertexIndexOffset = 1;  // VGT_INDX_OFFSET
    constexpr uint32_t kVertexIndexMin = 2;     // VGT_MIN_VTX_INDX
    constexpr uint32_t kVertexIndexMax = 3;     // VGT_MAX_VTX_INDX
    constexpr uint32_t kNdcScale = 4;           // float3
    constexpr uint32_t kPointSize = 7;          // float, tamanho padrao do ponto (PA_SU_POINT_SIZE)
    constexpr uint32_t kNdcOffset = 8;          // float3
    constexpr uint32_t kAlphaRef = 11;          // float
    constexpr uint32_t kAlphaFunc = 12;         // uint (CompareFunction)
    constexpr uint32_t kIndexRingOffset = 13;   // uint, indices no buffer de upload (rect lists)
    constexpr uint32_t kParamGenWindowOffset = 14;  // float2
    constexpr uint32_t kColorExpBias = 16;      // float4: multiplicador da saida de cada RT
    constexpr uint32_t kPointSizeMinMax = 20;   // float2 (PA_SU_POINT_MINMAX)
    constexpr uint32_t kGammaTargets = 22;      // uint: bit i = RT i guarda gamma (PWL)
    constexpr uint32_t kParamGenRegister = 23;  // uint: registrador do ps_param_gen, ou >= 64 se desligado

    // Bits de kFlags.
    constexpr uint32_t kFlagXYDividedByW = 1u << 0;
    constexpr uint32_t kFlagZDividedByW = 1u << 1;
    constexpr uint32_t kFlagWNotReciprocal = 1u << 2;
    constexpr uint32_t kFlagPrimitivePolygonal = 1u << 3;
    constexpr uint32_t kFlagPrimitiveLine = 1u << 4;
    constexpr uint32_t kFlagPrimitivePoint = 1u << 5;
    constexpr uint32_t kFlagAlphaTest = 1u << 6;
    constexpr uint32_t kFlagIndicesFromRing = 1u << 7;  // rect lists: indices em kIndexRingOffset
    constexpr uint32_t kFlagIndex32 = 1u << 8;          // indices de 32 bits no ring (senao 16)
}
