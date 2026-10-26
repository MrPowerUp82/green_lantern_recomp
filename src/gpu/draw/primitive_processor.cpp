#include "gpu/draw/primitive_processor.h"

#include <algorithm>

namespace Gpu::Draw {
    namespace {
        // Indices do guest com troca de endian, mascara de 24 bits e o indice de reset trocado
        // por kRestartIndex (VGT_MULTI_PRIM_IB_RESET_INDX, quando multi_prim_ib_ena).
        bool ReadGuestIndices(const DrawCall& draw, const GuestMemory& memory, std::vector<uint32_t>& out) {
            out.clear();
            if (!draw.indexed) {
                out.resize(draw.indexCount);
                for (uint32_t i = 0; i < draw.indexCount; ++i) out[i] = i;
                return true;
            }
            const uint32_t indexBytes = draw.indexFormat == xenos::IndexFormat::kInt16 ? 2 : 4;
            // O D3D as vezes declara VGT_DMA_SIZE menor do que num_indices: le so o que existe.
            const uint32_t available = draw.indexBufferBytes / indexBytes;
            const uint32_t count = std::min(draw.indexCount, available);
            if (count && !memory.IsReadable(draw.indexBase, uint64_t(count) * indexBytes)) return false;

            const auto modeCntl = draw.regs.Get<reg::PA_SU_SC_MODE_CNTL>();
            const auto resetReg = draw.regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>();
            const bool resetEnabled = modeCntl.multi_prim_ib_ena != 0;
            const uint32_t resetIndex = resetReg.reset_indx & (indexBytes == 2 ? 0xFFFFu : 0xFFFFFFu);

            out.resize(count);
            const uint8_t* base = memory.Base() + draw.indexBase;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t value;
                if (indexBytes == 2) {
                    uint16_t raw;
                    std::memcpy(&raw, base + i * 2, 2);
                    value = xenos::GpuSwap(raw, draw.indexEndian);
                } else {
                    uint32_t raw;
                    std::memcpy(&raw, base + i * 4, 4);
                    value = xenos::GpuSwap(raw, draw.indexEndian) & 0xFFFFFFu;
                }
                out[i] = (resetEnabled && value == resetIndex) ? kRestartIndex : value;
            }
            return true;
        }

        bool HasRestart(const std::vector<uint32_t>& indices) {
            return std::find(indices.begin(), indices.end(), kRestartIndex) != indices.end();
        }

        // Divide a lista em trechos separados por kRestartIndex.
        template <typename Fn>
        void ForEachSegment(const std::vector<uint32_t>& indices, Fn&& fn) {
            size_t start = 0;
            for (size_t i = 0; i <= indices.size(); ++i) {
                if (i == indices.size() || indices[i] == kRestartIndex) {
                    if (i > start) fn(&indices[start], i - start);
                    start = i + 1;
                }
            }
        }
    }

    HostPrimitive ProcessPrimitive(const DrawCall& draw, const GuestMemory& memory) {
        HostPrimitive result;
        std::vector<uint32_t> guest;
        if (!ReadGuestIndices(draw, memory, guest)) return result;

        // Lista nativa: sem indices no host se o draw nao era indexado.
        auto passthrough = [&](HostTopology topology) {
            result.topology = topology;
            result.indexed = draw.indexed;
            if (draw.indexed) {
                result.indices = std::move(guest);
            } else {
                result.vertexCount = draw.indexCount;
            }
            result.ok = true;
        };

        switch (draw.primitive) {
        case xenos::PrimitiveType::kPointList:
            result.points = true;
            passthrough(HostTopology::kPointList);
            break;
        case xenos::PrimitiveType::kLineList:
            result.lines = true;
            passthrough(HostTopology::kLineList);
            break;
        case xenos::PrimitiveType::kLineStrip:
            result.lines = true;
            passthrough(HostTopology::kLineStrip);
            break;
        case xenos::PrimitiveType::kTriangleList:
        case xenos::PrimitiveType::kTriangleWithWFlags:
            result.polygonal = true;
            passthrough(HostTopology::kTriangleList);
            break;
        case xenos::PrimitiveType::kTriangleStrip:
            result.polygonal = true;
            passthrough(HostTopology::kTriangleStrip);
            break;

        case xenos::PrimitiveType::kTriangleFan:
        case xenos::PrimitiveType::kPolygon:
            result.polygonal = true;
            if (!HasRestart(guest)) {
                passthrough(HostTopology::kTriangleFan);
                break;
            }
            // O host so reinicia strips: leques com reset viram listas.
            result.topology = HostTopology::kTriangleList;
            result.indexed = true;
            ForEachSegment(guest, [&](const uint32_t* s, size_t n) {
                for (size_t i = 2; i < n; ++i) result.indices.insert(result.indices.end(), { s[0], s[i - 1], s[i] });
            });
            result.ok = true;
            break;

        case xenos::PrimitiveType::kLineLoop:
            result.lines = true;
            result.topology = HostTopology::kLineList;
            result.indexed = true;
            ForEachSegment(guest, [&](const uint32_t* s, size_t n) {
                if (n < 2) return;
                for (size_t i = 0; i < n; ++i) result.indices.insert(result.indices.end(), { s[i], s[(i + 1) % n] });
            });
            result.ok = true;
            break;

        case xenos::PrimitiveType::kQuadList:
            result.polygonal = true;
            result.topology = HostTopology::kTriangleList;
            result.indexed = true;
            for (size_t q = 0; q + 3 < guest.size(); q += 4) {
                const uint32_t* v = &guest[q];
                result.indices.insert(result.indices.end(), { v[0], v[1], v[2], v[0], v[2], v[3] });
            }
            result.ok = true;
            break;

        case xenos::PrimitiveType::kQuadStrip:
            result.polygonal = true;
            result.topology = HostTopology::kTriangleList;
            result.indexed = true;
            ForEachSegment(guest, [&](const uint32_t* s, size_t n) {
                for (size_t i = 0; i + 3 < n; i += 2) {
                    result.indices.insert(result.indices.end(), { s[i], s[i + 1], s[i + 3], s[i], s[i + 3], s[i + 2] });
                }
            });
            result.ok = true;
            break;

        case xenos::PrimitiveType::kRectangleList: {
            result.polygonal = true;
            result.rectangleList = true;
            result.topology = HostTopology::kTriangleList;
            const size_t rects = guest.size() / 3;
            guest.resize(rects * 3);
            result.indices = std::move(guest);
            result.vertexCount = uint32_t(rects * 6);
            result.ok = rects != 0;
            break;
        }

        default:
            // Patches de tesselacao e primitivas 2D explicitas nao sao usadas pelo D3D do jogo.
            return result;
        }
        if (result.indexed && result.indices.empty()) result.ok = false;
        if (!result.indexed && !result.rectangleList && result.vertexCount == 0) result.ok = false;
        return result;
    }
}
