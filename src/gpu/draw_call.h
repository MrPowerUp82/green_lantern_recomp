#pragma once

#include "gpu/gpu_registers.h"
#include "types.h"

#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <vector>

namespace Gpu {
    namespace xenos = xe::gpu::xenos;
    namespace reg = xe::gpu::reg;

    // Leitura do register file do CP. So a thread do CP escreve nele, e o backend le na mesma
    // thread durante IssueDraw/IssueCopy/Swap, entao os valores ficam estaveis durante a chamada.
    class RegisterFileView {
    public:
        explicit RegisterFileView(const std::atomic<uint32_t>* regs = nullptr) : m_regs(regs) {}

        uint32_t operator[](uint32_t index) const {
            return (m_regs && index < Reg::kRegisterCount) ? m_regs[index].load(std::memory_order_relaxed) : 0;
        }
        float GetFloat(uint32_t index) const { return std::bit_cast<float>((*this)[index]); }

        // Registradores tipados do Xenia (xe::gpu::reg::*), pelo indice padrao ou um indice dado
        // (ex.: RB_COLOR_INFO de cada render target).
        template <typename T>
        T Get() const {
            T value;
            value.value = (*this)[T::register_index];
            return value;
        }
        template <typename T>
        T Get(uint32_t index) const {
            T value;
            value.value = (*this)[index];
            return value;
        }

        // Fetch constants de vertice (3 por slot de 6 dwords) e de textura (6 dwords).
        xenos::xe_gpu_vertex_fetch_t VertexFetch(uint32_t index) const {
            xenos::xe_gpu_vertex_fetch_t fetch;
            const uint32_t base = xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + index * 2;
            fetch.dword_0 = (*this)[base];
            fetch.dword_1 = (*this)[base + 1];
            return fetch;
        }
        xenos::xe_gpu_texture_fetch_t TextureFetch(uint32_t index) const {
            xenos::xe_gpu_texture_fetch_t fetch;
            const uint32_t base = xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + index * 6;
            fetch.dword_0 = (*this)[base];
            fetch.dword_1 = (*this)[base + 1];
            fetch.dword_2 = (*this)[base + 2];
            fetch.dword_3 = (*this)[base + 3];
            fetch.dword_4 = (*this)[base + 4];
            fetch.dword_5 = (*this)[base + 5];
            return fetch;
        }

    private:
        const std::atomic<uint32_t>* m_regs;
    };

    // Microcodigo carregado por IM_LOAD / IM_LOAD_IMMEDIATE (dwords ja na ordem do host).
    struct ShaderBlob {
        xenos::ShaderType type = xenos::ShaderType::kVertex;
        uint64_t hash = 0;  // do microcodigo e do tipo
        std::vector<uint32_t> ucode;
    };

    uint64_t HashShaderUcode(xenos::ShaderType type, const uint32_t* ucode, uint32_t dwordCount);

    // Um DRAW_INDX/DRAW_INDX_2 em modo de render (RB_MODECONTROL != kCopy).
    struct DrawCall {
        RegisterFileView regs;
        const ShaderBlob* vertexShader = nullptr;
        const ShaderBlob* pixelShader = nullptr;  // nulo se o jogo nunca carregou um
        xenos::PrimitiveType primitive = xenos::PrimitiveType::kNone;
        uint32_t indexCount = 0;  // VGT_DRAW_INITIATOR.num_indices

        // Draw indexado (VGT_DMA): indices no guest; senao os indices sao 0..indexCount-1.
        bool indexed = false;
        GuestAddr indexBase = 0;
        xenos::IndexFormat indexFormat = xenos::IndexFormat::kInt16;
        xenos::Endian indexEndian = xenos::Endian::kNone;
        uint32_t indexBufferBytes = 0;  // VGT_DMA_SIZE.num_words * tamanho do indice
    };
}
