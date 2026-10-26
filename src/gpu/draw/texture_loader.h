#pragma once

#include "gpu/guest_memory.h"

#include "xenia/gpu/xenos.h"

#include <cstdint>
#include <vector>

// Leitura de texturas do guest na CPU: subresources a partir da fetch constant, detiling (enderecos
// de xenia/gpu/texture_address.h), troca de endian, mips empacotados e conversao para um formato do host.
namespace Gpu::Draw {
    namespace xenos = xe::gpu::xenos;

    enum class HostTextureFormat {
        kUnknown,
        kR8Unorm,
        kR8G8Unorm,
        kR8G8B8A8Unorm,
        kR8Snorm,
        kR8G8Snorm,
        kR8G8B8A8Snorm,
        kR16Unorm,
        kR16G16Unorm,
        kR16G16B16A16Unorm,
        kR16Snorm,
        kR16G16Snorm,
        kR16G16B16A16Snorm,
        kR16Float,
        kR16G16Float,
        kR16G16B16A16Float,
        kR32Float,
        kR32G32Float,
        kR32G32B32A32Float,
        kBC1,
        kBC2,
        kBC3,
        kBC4,
        kBC5,
        kBC5Snorm,
    };

    // Tamanho do bloco do formato do host (4x4 para BC, 1x1 nos demais) e bytes por bloco.
    uint32_t HostFormatBlockSize(HostTextureFormat format);
    uint32_t HostFormatBytesPerBlock(HostTextureFormat format);

    struct TextureDescription {
        bool valid = false;
        xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
        xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;  // formato base (sem _AS_/_EXPAND)
        xenos::Endian endian = xenos::Endian::kNone;
        uint32_t width = 1, height = 1;
        uint32_t depth = 1;   // fatias de textura 3D
        uint32_t layers = 1;  // camadas de textura empilhada (stacked) ou 6 faces de cubo
        uint32_t mipLevels = 1;
        uint32_t pitchDiv32 = 0;
        bool tiled = false;
        bool packedMips = false;
        bool signedComponents = false;  // todos os componentes com sinal (TextureSign::kSigned)
        GuestAddr baseAddress = 0;  // nivel 0 (0 se ausente)
        GuestAddr mipAddress = 0;   // niveis 1..
        uint32_t baseExtentBytes = 0;
        uint32_t mipExtentBytes = 0;
    };

    // Le a fetch constant. As faixas de memoria (base/mip) servem para validar o cache.
    TextureDescription DescribeTexture(const xenos::xe_gpu_texture_fetch_t& fetch);

    struct DecodedSubresource {
        uint32_t level = 0;
        uint32_t layer = 0;  // camada (stacked/cubo)
        uint32_t width = 0, height = 0, depth = 1;  // em texels
        uint32_t rowBytes = 0;   // bytes por linha de blocos
        uint32_t rowCount = 0;   // linhas de blocos por fatia
        size_t offset = 0;       // em `data`, alinhado a 16
    };

    struct DecodedTexture {
        bool ok = false;
        HostTextureFormat format = HostTextureFormat::kUnknown;
        TextureDescription desc;
        std::vector<uint8_t> data;
        std::vector<DecodedSubresource> subresources;
    };

    DecodedTexture DecodeTexture(const TextureDescription& desc, const GuestMemory& memory);

    // Hash do conteudo do guest usado pela textura (para detectar escritas da CPU).
    uint64_t HashTextureMemory(const TextureDescription& desc, const GuestMemory& memory);

    // Hash rapido de 64 bits de uma faixa de bytes.
    uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0);
}
