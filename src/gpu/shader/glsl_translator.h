#pragma once

#include "xenia/gpu/xenos.h"

#include <cstdint>
#include <string>
#include <vector>

// Traducao do microcodigo Xenos (formato do Xenia em xenia/gpu/ucode.h) para GLSL 4.50 Vulkan.
// O GLSL e compilado para SPIR-V pelo renderer (glslang). As semanticas das instrucoes seguem o
// interpretador e o tradutor SPIR-V do Xenia.
//
// Interface com o renderer:
// - set 0, binding 0: SSBO com o buffer de upload; o push constant `cb` aponta para o bloco de
//   constantes do draw (draw_constants.h). Os vertex fetches leem os dados de vertice do mesmo SSBO.
// - set 1: para cada TextureBinding i, a imagem em base+3*i (texture2D, texture3D ou textureCube
//   conforme a dimensao), o sampler em base+3*i+1 e, so para k3DOrStacked, a alternativa empilhada
//   (texture2DArray) em base+3*i+2; o shader escolhe pela dimensao da fetch constant. base = 0 no
//   VS e kPixelTextureBindingBase no PS, para os dois estagios dividirem a mesma set.
// - VS -> PS: 16 interpoladores em location 0..15.
namespace Gpu::Shader {
    namespace xenos = xe::gpu::xenos;

    constexpr uint32_t kPixelTextureBindingBase = 96;  // 32 fetch constants x 3 bindings

    enum class VertexShaderMode {
        kNormal,
        // Rect lists: o host desenha 6 vertices por retangulo e o shader calcula o 4o canto
        // executando o programa do guest para os 3 vertices do retangulo.
        kRectangleList,
    };

    struct TextureBinding {
        uint32_t fetchConstant = 0;
        xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k2D;
        // Sobrescritas de filtro da instrucao (kUseFetchConst = usa a fetch constant).
        xenos::TextureFilter magFilter = xenos::TextureFilter::kUseFetchConst;
        xenos::TextureFilter minFilter = xenos::TextureFilter::kUseFetchConst;
        xenos::TextureFilter mipFilter = xenos::TextureFilter::kUseFetchConst;
        xenos::AnisoFilter anisoFilter = xenos::AnisoFilter::kUseFetchConst;

        bool operator==(const TextureBinding&) const = default;
    };

    struct TranslatedShader {
        bool ok = false;
        std::string error;
        xenos::ShaderType type = xenos::ShaderType::kVertex;
        std::string glsl;

        std::vector<TextureBinding> textures;
        uint32_t vertexFetchMask[3] = {};  // bits das 96 vertex fetch constants usadas
        uint32_t interpolatorsWritten = 0; // VS: bit i = escreve o interpolador i
        uint32_t colorTargetsWritten = 0;  // PS: bit i = escreve oC[i]
        bool writesDepth = false;
        bool killsPixels = false;
        bool writesPointSize = false;
        bool usesMemexport = false;        // eA/eM: ignorado (contado em unsupported)
        uint32_t unsupported = 0;          // instrucoes que viraram no-op
        uint32_t instructionCount = 0;
    };

    TranslatedShader TranslateShader(const uint32_t* ucode, uint32_t dwordCount, xenos::ShaderType type,
                                     VertexShaderMode vertexMode = VertexShaderMode::kNormal);
}
