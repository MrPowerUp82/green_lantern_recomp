#pragma once

#include "types.h"

#include <atomic>
#include <cstdint>

namespace Gpu {
    // Cena de verificacao do caminho completo de draw, como o D3D do 360 a emitiria:
    // microcodigo Xenos (IM_LOAD_IMMEDIATE), vertex buffer e textura tiled na memoria do guest,
    // clear de cor e profundidade por resolve, um triangulo texturizado, um triangulo atras dele
    // (teste de profundidade), uma rect list (4o vertice calculado no VS), resolve de cor para o
    // frontbuffer e de profundidade para uma textura, seguidos de VdSwap. Exige GpuSystem inicializado.
    struct ScenePixel {
        uint32_t x, y;
        uint8_t rgba[4];
    };

    struct SceneDepth {
        uint32_t x, y;
        float depth;
    };

    struct SceneResult {
        bool ok = false;
        uint32_t framesSubmitted = 0;
        GuestAddr frontbuffer = 0;
        GuestAddr depthTexture = 0;  // resolve da profundidade (lido como R32F)
        uint32_t width = 1280;
        uint32_t height = 720;
        // Pixels esperados no frontbuffer: metades do triangulo texturizado, a parte visivel do
        // triangulo de tras (teste de profundidade), a sobreposicao (o da frente vence), as duas
        // metades da rect list e um canto.
        ScenePixel expected[7] = {};
        // Profundidades esperadas no resolve de profundidade.
        SceneDepth expectedDepth[3] = {};
    };

    SceneResult RunDrawSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel = nullptr);
}
