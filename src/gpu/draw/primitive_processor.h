#pragma once

#include "gpu/draw_call.h"
#include "gpu/guest_memory.h"

#include <cstdint>
#include <vector>

namespace Gpu::Draw {
    // Topologias que o host desenha diretamente (as demais do Xenos sao convertidas).
    enum class HostTopology {
        kPointList,
        kLineList,
        kLineStrip,
        kTriangleList,
        kTriangleStrip,
        kTriangleFan,
    };

    constexpr uint32_t kRestartIndex = 0xFFFFFFFFu;

    // Resultado da conversao de um draw do Xenos para o host.
    struct HostPrimitive {
        bool ok = false;
        HostTopology topology = HostTopology::kTriangleList;
        // Rect list: o host desenha `vertexCount` (= 6 por retangulo) vertices sem indices, e o VS
        // le os indices do guest de `indices` (3 por retangulo) no buffer de upload.
        bool rectangleList = false;
        // Com indices: `indices` vira o index buffer (32 bits) e o host desenha indices.size().
        // Sem indices: o host desenha `vertexCount` vertices e gl_VertexIndex e o indice do guest.
        bool indexed = false;
        std::vector<uint32_t> indices;  // indices do guest (24 bits) ou kRestartIndex
        uint32_t vertexCount = 0;
        bool polygonal = false;  // triangulos (face/culling importam)
        bool lines = false;
        bool points = false;
    };

    // Le e converte os indices de um draw. A memoria e o espaco de 4 GB do guest.
    HostPrimitive ProcessPrimitive(const DrawCall& draw, const GuestMemory& memory);
}
