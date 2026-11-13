#include "gpu/draw/texture_loader.h"

#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Gpu::Draw {
    namespace texture_util = xe::gpu::texture_util;
    namespace texture_address = xe::gpu::texture_address;
    using xe::gpu::FormatInfo;
    using xe::gpu::FormatType;

    namespace {
        HostTextureFormat ChooseHostFormat(xenos::TextureFormat format, bool isSigned) {
            using F = xenos::TextureFormat;
            using H = HostTextureFormat;
            switch (format) {
            case F::k_8: return isSigned ? H::kR8Snorm : H::kR8Unorm;
            case F::k_8_8: return isSigned ? H::kR8G8Snorm : H::kR8G8Unorm;
            case F::k_8_8_8_8: return isSigned ? H::kR8G8B8A8Snorm : H::kR8G8B8A8Unorm;
            case F::k_1_5_5_5:
            case F::k_5_6_5:
            case F::k_6_5_5:
            case F::k_4_4_4_4:
            case F::k_Y1_Cr_Y0_Cb_REP:
            case F::k_Cr_Y1_Cb_Y0_REP:
                return H::kR8G8B8A8Unorm;
            case F::k_2_10_10_10:
            case F::k_10_11_11:
            case F::k_11_11_10:
                return isSigned ? H::kR16G16B16A16Snorm : H::kR16G16B16A16Unorm;
            case F::k_DXT1: return H::kBC1;
            case F::k_DXT2_3: return H::kBC2;
            case F::k_DXT4_5: return H::kBC3;
            case F::k_DXT5A: return H::kBC4;
            case F::k_DXN: return isSigned ? H::kBC5Snorm : H::kBC5;
            case F::k_DXT3A: return H::kR8Unorm;
            case F::k_CTX1: return H::kR8G8Unorm;
            case F::k_16: return isSigned ? H::kR16Snorm : H::kR16Unorm;
            case F::k_16_16: return isSigned ? H::kR16G16Snorm : H::kR16G16Unorm;
            case F::k_16_16_16_16: return isSigned ? H::kR16G16B16A16Snorm : H::kR16G16B16A16Unorm;
            case F::k_16_16_EDRAM: return H::kR16G16Snorm;
            case F::k_16_16_16_16_EDRAM: return H::kR16G16B16A16Snorm;
            case F::k_16_FLOAT: return H::kR16Float;
            case F::k_16_16_FLOAT: return H::kR16G16Float;
            case F::k_16_16_16_16_FLOAT: return H::kR16G16B16A16Float;
            case F::k_2_10_10_10_FLOAT_EDRAM: return H::kR16G16B16A16Float;
            case F::k_32:
            case F::k_32_FLOAT:
            case F::k_24_8:
            case F::k_24_8_FLOAT:
                return H::kR32Float;
            case F::k_32_32:
            case F::k_32_32_FLOAT:
                return H::kR32G32Float;
            case F::k_32_32_32_32:
            case F::k_32_32_32_32_FLOAT:
            case F::k_32_32_32_FLOAT:
                return H::kR32G32B32A32Float;
            default:
                return H::kUnknown;
            }
        }

        uint16_t FloatToHalf(float f) {
            const uint32_t x = std::bit_cast<uint32_t>(f);
            const uint32_t sign = (x >> 16) & 0x8000;
            int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
            uint32_t mant = x & 0x7FFFFF;
            if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (mant ? 0x200 : 0));
            if (exp >= 31) return uint16_t(sign | 0x7C00);
            if (exp <= 0) {
                if (exp < -10) return uint16_t(sign);
                mant |= 0x800000;
                const uint32_t shift = uint32_t(14 - exp);
                return uint16_t(sign | (mant >> shift));
            }
            return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
        }

        uint16_t Unorm16(uint32_t value, uint32_t bits) {
            return uint16_t((uint64_t(value) * 65535u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
        }

        uint8_t Expand(uint32_t value, uint32_t bits) {
            return uint8_t((value * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
        }

        int16_t Snorm16(int32_t value, uint32_t bits) {
            const int32_t maxv = (1 << (bits - 1)) - 1;
            const float f = std::max(-1.0f, float(value) / float(maxv));
            return int16_t(f * 32767.0f);
        }

        uint32_t Bits(uint32_t v, uint32_t offset, uint32_t width) { return (v >> offset) & ((1u << width) - 1); }
        int32_t SignedBits(uint32_t v, uint32_t offset, uint32_t width) {
            return int32_t(v << (32 - offset - width)) >> (32 - width);
        }

        uint8_t Clamp8(float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

        // Converte uma linha de blocos do guest (ja com endian do host) para o formato do host.
        void ConvertRow(xenos::TextureFormat format, bool isSigned, const uint8_t* src, uint8_t* dst, uint32_t blocks) {
            using F = xenos::TextureFormat;
            switch (format) {
            case F::k_1_5_5_5:
            case F::k_5_6_5:
            case F::k_6_5_5:
            case F::k_4_4_4_4:
                for (uint32_t i = 0; i < blocks; ++i) {
                    uint16_t v;
                    std::memcpy(&v, src + i * 2, 2);
                    uint8_t* o = dst + i * 4;
                    switch (format) {
                    case F::k_1_5_5_5:
                        o[0] = Expand(Bits(v, 0, 5), 5); o[1] = Expand(Bits(v, 5, 5), 5);
                        o[2] = Expand(Bits(v, 10, 5), 5); o[3] = Bits(v, 15, 1) ? 255 : 0;
                        break;
                    case F::k_5_6_5:
                        o[0] = Expand(Bits(v, 0, 5), 5); o[1] = Expand(Bits(v, 5, 6), 6);
                        o[2] = Expand(Bits(v, 11, 5), 5); o[3] = 255;
                        break;
                    case F::k_6_5_5:
                        o[0] = Expand(Bits(v, 0, 6), 6); o[1] = Expand(Bits(v, 6, 5), 5);
                        o[2] = Expand(Bits(v, 11, 5), 5); o[3] = 255;
                        break;
                    default:
                        o[0] = Expand(Bits(v, 0, 4), 4); o[1] = Expand(Bits(v, 4, 4), 4);
                        o[2] = Expand(Bits(v, 8, 4), 4); o[3] = Expand(Bits(v, 12, 4), 4);
                        break;
                    }
                }
                break;

            case F::k_2_10_10_10:
            case F::k_10_11_11:
            case F::k_11_11_10: {
                uint32_t widths[4], offsets[4];
                if (format == F::k_2_10_10_10) {
                    widths[0] = widths[1] = widths[2] = 10; widths[3] = 2;
                    offsets[0] = 0; offsets[1] = 10; offsets[2] = 20; offsets[3] = 30;
                } else if (format == F::k_10_11_11) {
                    widths[0] = widths[1] = 11; widths[2] = 10; widths[3] = 0;
                    offsets[0] = 0; offsets[1] = 11; offsets[2] = 22; offsets[3] = 0;
                } else {
                    widths[0] = 10; widths[1] = widths[2] = 11; widths[3] = 0;
                    offsets[0] = 0; offsets[1] = 10; offsets[2] = 21; offsets[3] = 0;
                }
                for (uint32_t i = 0; i < blocks; ++i) {
                    uint32_t v;
                    std::memcpy(&v, src + i * 4, 4);
                    uint16_t o[4];
                    for (int c = 0; c < 4; ++c) {
                        if (!widths[c]) {
                            o[c] = isSigned ? 32767 : 65535;
                        } else if (isSigned) {
                            o[c] = uint16_t(Snorm16(SignedBits(v, offsets[c], widths[c]), widths[c]));
                        } else {
                            o[c] = Unorm16(Bits(v, offsets[c], widths[c]), widths[c]);
                        }
                    }
                    std::memcpy(dst + i * 8, o, 8);
                }
                break;
            }

            case F::k_2_10_10_10_FLOAT_EDRAM:
                for (uint32_t i = 0; i < blocks; ++i) {
                    uint32_t v;
                    std::memcpy(&v, src + i * 4, 4);
                    uint16_t o[4];
                    for (int c = 0; c < 3; ++c) o[c] = FloatToHalf(xenos::Float7e3To32(Bits(v, c * 10, 10)));
                    o[3] = FloatToHalf(float(Bits(v, 30, 2)) / 3.0f);
                    std::memcpy(dst + i * 8, o, 8);
                }
                break;

            case F::k_24_8:
            case F::k_24_8_FLOAT:
                for (uint32_t i = 0; i < blocks; ++i) {
                    uint32_t v;
                    std::memcpy(&v, src + i * 4, 4);
                    const float depth = format == F::k_24_8 ? float(v >> 8) / 16777215.0f
                                                            : xenos::Float20e4To32(v >> 8);
                    std::memcpy(dst + i * 4, &depth, 4);
                }
                break;

            case F::k_32_32_32_FLOAT:
                for (uint32_t i = 0; i < blocks; ++i) {
                    std::memcpy(dst + i * 16, src + i * 12, 12);
                    const float one = 1.0f;
                    std::memcpy(dst + i * 16 + 12, &one, 4);
                }
                break;

            case F::k_DXT3A:
                // Bloco 4x4 de alfa de 4 bits (como a metade de alfa do BC2) -> R8, 4 linhas por bloco.
                // O destino aqui e uma "linha de blocos" de 4 linhas de texels (tratada pelo chamador).
                break;

            case F::k_Y1_Cr_Y0_Cb_REP:
            case F::k_Cr_Y1_Cb_Y0_REP:
                // Bloco 2x1 (4 bytes): dois texels compartilhando crominancia (BT.601).
                for (uint32_t i = 0; i < blocks; ++i) {
                    const uint8_t* b = src + i * 4;
                    float y0, y1, cb, cr;
                    if (format == F::k_Y1_Cr_Y0_Cb_REP) {
                        cb = b[0]; y0 = b[1]; cr = b[2]; y1 = b[3];
                    } else {
                        y0 = b[0]; cb = b[1]; y1 = b[2]; cr = b[3];
                    }
                    for (int t = 0; t < 2; ++t) {
                        const float y = 1.164f * ((t ? y1 : y0) - 16.0f);
                        uint8_t* o = dst + i * 8 + t * 4;
                        o[0] = Clamp8(y + 1.596f * (cr - 128.0f));
                        o[1] = Clamp8(y - 0.813f * (cr - 128.0f) - 0.391f * (cb - 128.0f));
                        o[2] = Clamp8(y + 2.018f * (cb - 128.0f));
                        o[3] = 255;
                    }
                }
                break;

            default:
                break;
            }
        }

        // Formatos copiados sem conversao (o host le os mesmos bytes).
        bool IsPassthrough(xenos::TextureFormat format) {
            using F = xenos::TextureFormat;
            switch (format) {
            case F::k_8: case F::k_8_8: case F::k_8_8_8_8:
            case F::k_DXT1: case F::k_DXT2_3: case F::k_DXT4_5: case F::k_DXT5A: case F::k_DXN:
            case F::k_16: case F::k_16_16: case F::k_16_16_16_16:
            case F::k_16_16_EDRAM: case F::k_16_16_16_16_EDRAM:
            case F::k_16_FLOAT: case F::k_16_16_FLOAT: case F::k_16_16_16_16_FLOAT:
            case F::k_32: case F::k_32_32: case F::k_32_32_32_32:
            case F::k_32_FLOAT: case F::k_32_32_FLOAT: case F::k_32_32_32_32_FLOAT:
                return true;
            default:
                return false;
            }
        }

        // Decodifica blocos 4x4 de 64 bits (DXT3A -> R8, CTX1 -> R8G8) para texels.
        void DecodeBlock4x4(xenos::TextureFormat format, const uint8_t* block, uint8_t* dst, uint32_t dstRowBytes,
                            uint32_t texelBytes) {
            if (format == xenos::TextureFormat::k_DXT3A) {
                for (uint32_t y = 0; y < 4; ++y) {
                    uint16_t row;
                    std::memcpy(&row, block + y * 2, 2);
                    for (uint32_t x = 0; x < 4; ++x) dst[y * dstRowBytes + x] = uint8_t(((row >> (x * 4)) & 0xF) * 17);
                }
                return;
            }
            // CTX1: dois endpoints (x, y de 8 bits) e indices de 2 bits.
            const uint8_t e0x = block[0], e0y = block[1], e1x = block[2], e1y = block[3];
            uint32_t indices;
            std::memcpy(&indices, block + 4, 4);
            const uint8_t px[4] = { e0x, e1x, uint8_t((2 * e0x + e1x) / 3), uint8_t((e0x + 2 * e1x) / 3) };
            const uint8_t py[4] = { e0y, e1y, uint8_t((2 * e0y + e1y) / 3), uint8_t((e0y + 2 * e1y) / 3) };
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t idx = (indices >> ((y * 4 + x) * 2)) & 3;
                    uint8_t* o = dst + y * dstRowBytes + x * texelBytes;
                    o[0] = px[idx];
                    o[1] = py[idx];
                }
            }
        }
    }

    uint32_t HostFormatBlockSize(HostTextureFormat format) {
        switch (format) {
        case HostTextureFormat::kBC1: case HostTextureFormat::kBC2: case HostTextureFormat::kBC3:
        case HostTextureFormat::kBC4: case HostTextureFormat::kBC5: case HostTextureFormat::kBC5Snorm:
            return 4;
        default:
            return 1;
        }
    }

    uint32_t HostFormatBytesPerBlock(HostTextureFormat format) {
        using H = HostTextureFormat;
        switch (format) {
        case H::kR8Unorm: case H::kR8Snorm: return 1;
        case H::kR8G8Unorm: case H::kR8G8Snorm: case H::kR16Unorm: case H::kR16Snorm: case H::kR16Float: return 2;
        case H::kR8G8B8A8Unorm: case H::kR8G8B8A8Snorm: case H::kR16G16Unorm: case H::kR16G16Snorm:
        case H::kR16G16Float: case H::kR32Float:
            return 4;
        case H::kR16G16B16A16Unorm: case H::kR16G16B16A16Snorm: case H::kR16G16B16A16Float: case H::kR32G32Float:
        case H::kBC1: case H::kBC4:
            return 8;
        case H::kR32G32B32A32Float: case H::kBC2: case H::kBC3: case H::kBC5: case H::kBC5Snorm:
            return 16;
        default:
            return 0;
        }
    }

    TextureDescription DescribeTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
        TextureDescription desc;
        if (fetch.type != xenos::FetchConstantType::kTexture) return desc;
        desc.dimension = fetch.dimension;
        desc.format = xe::gpu::GetBaseFormat(fetch.format);
        desc.endian = fetch.endianness;
        desc.tiled = fetch.tiled != 0;
        desc.packedMips = fetch.packed_mips != 0;
        desc.pitchDiv32 = fetch.pitch;
        desc.signedComponents = fetch.sign_x == xenos::TextureSign::kSigned;

        uint32_t widthMinus1 = 0, heightMinus1 = 0, depthMinus1 = 0;
        switch (fetch.dimension) {
        case xenos::DataDimension::k1D:
            widthMinus1 = fetch.size_1d.width;
            break;
        case xenos::DataDimension::k2DOrStacked:
            widthMinus1 = fetch.size_2d.width;
            heightMinus1 = fetch.size_2d.height;
            depthMinus1 = fetch.stacked ? fetch.size_2d.stack_depth : 0;
            break;
        case xenos::DataDimension::k3D:
            widthMinus1 = fetch.size_3d.width;
            heightMinus1 = fetch.size_3d.height;
            depthMinus1 = fetch.size_3d.depth;
            break;
        case xenos::DataDimension::kCube:
            widthMinus1 = fetch.size_2d.width;
            heightMinus1 = fetch.size_2d.height;
            depthMinus1 = 5;
            break;
        }
        desc.width = widthMinus1 + 1;
        desc.height = heightMinus1 + 1;
        // Texturas 1D muito largas viram linhas de no maximo 8192 texels.
        if (fetch.dimension == xenos::DataDimension::k1D && desc.width > xenos::kTexture2DCubeMaxWidthHeight) {
            desc.height = std::min((desc.width + xenos::kTexture2DCubeMaxWidthHeight - 1) /
                                       xenos::kTexture2DCubeMaxWidthHeight,
                                   xenos::kTexture1DWideMaxRows);
            desc.width = xenos::kTexture2DCubeMaxWidthHeight;
        }
        if (fetch.dimension == xenos::DataDimension::k3D) {
            desc.depth = depthMinus1 + 1;
        } else {
            desc.layers = depthMinus1 + 1;
        }

        // Faixa de mips como em GetSubresourcesFromFetchConstant do Xenia, mas sem mascarar os
        // enderecos para 512 MB: neste port o endereco da GPU e o endereco virtual do guest.
        uint32_t longestMinus1 = std::max(widthMinus1, heightMinus1);
        if (fetch.dimension == xenos::DataDimension::k3D) longestMinus1 = std::max(longestMinus1, depthMinus1);
        const uint32_t sizeMaxLevel = xe::log2_floor(longestMinus1 + 1);
        uint32_t basePage = fetch.base_address;
        uint32_t mipPage = fetch.mip_address;
        if (basePage && desc.packedMips && fetch.dimension != xenos::DataDimension::k1D &&
            texture_util::GetPackedMipLevel(desc.width, desc.height) == 0) {
            mipPage = basePage;
        }
        uint32_t mipMax = 0;
        if (mipPage) mipMax = std::min(uint32_t(fetch.mip_max_level), sizeMaxLevel);
        if (!basePage || !FormatInfo::Get(desc.format)) return desc;

        desc.baseAddress = basePage << 12;
        desc.mipAddress = mipMax ? (mipPage << 12) : 0;
        desc.mipLevels = mipMax + 1;

        const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
            fetch.dimension, desc.pitchDiv32, desc.width, desc.height,
            fetch.dimension == xenos::DataDimension::k3D ? desc.depth : desc.layers, desc.tiled, desc.format,
            desc.packedMips, true, mipMax);
        desc.baseExtentBytes = layout.base.level_data_extent_bytes;
        desc.mipExtentBytes = mipMax ? layout.mips_total_extent_bytes : 0;
        desc.valid = desc.baseExtentBytes != 0;
        return desc;
    }

    DecodedTexture DecodeTexture(const TextureDescription& desc, const GuestMemory& memory) {
        DecodedTexture out;
        out.desc = desc;
        if (!desc.valid) return out;
        const FormatInfo* info = FormatInfo::Get(desc.format);
        out.format = ChooseHostFormat(desc.format, desc.signedComponents);
        if (!info || out.format == HostTextureFormat::kUnknown) return out;

        const uint32_t guestBlockBytes = info->bytes_per_block();
        if (!guestBlockBytes) return out;
        const uint32_t bpbLog2 = xe::log2_floor(guestBlockBytes);
        const uint32_t blockW = info->block_width, blockH = info->block_height;
        const bool is3D = desc.dimension == xenos::DataDimension::k3D;
        const uint32_t depthOrLayers = is3D ? desc.depth : desc.layers;
        const texture_util::TextureGuestLayout layout =
            texture_util::GetGuestTextureLayout(desc.dimension, desc.pitchDiv32, desc.width, desc.height, depthOrLayers,
                                                desc.tiled, desc.format, desc.packedMips, true, desc.mipLevels - 1);

        const uint32_t hostBlock = HostFormatBlockSize(out.format);
        const uint32_t hostBlockBytes = HostFormatBytesPerBlock(out.format);
        // Formatos 4x4 decodificados para texels (DXT3A, CTX1): o guest e em blocos, o host em texels.
        const bool decode4x4 = desc.format == xenos::TextureFormat::k_DXT3A || desc.format == xenos::TextureFormat::k_CTX1;
        // Formatos 2x1 (YUV): um bloco do guest vira 2 texels.
        const bool yuv = desc.format == xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP ||
                         desc.format == xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP;

        std::vector<uint8_t> blockRow;
        for (uint32_t level = 0; level < desc.mipLevels; ++level) {
            const uint32_t width = std::max(desc.width >> level, 1u);
            const uint32_t height = std::max(desc.height >> level, 1u);
            const uint32_t depth = is3D ? std::max(desc.depth >> level, 1u) : 1u;
            const uint32_t blocksX = (width + blockW - 1) / blockW;
            const uint32_t blocksY = (height + blockH - 1) / blockH;

            // Nivel no guest: base no endereco base; mips no endereco de mips. O que estiver na
            // cauda empacotada fica no nivel `packed_level` com um offset em blocos.
            const bool packed = level >= layout.packed_level;
            const uint32_t storedLevel = packed ? layout.packed_level : level;
            const bool inBase = storedLevel == 0;
            const texture_util::TextureGuestLayout::Level& guestLevel = inBase ? layout.base : layout.mips[storedLevel];
            const GuestAddr levelAddress =
                inBase ? desc.baseAddress : desc.mipAddress + layout.mip_offsets_bytes[storedLevel];
            uint32_t packedX = 0, packedY = 0, packedZ = 0;
            if (packed) {
                texture_util::GetPackedMipOffset(desc.width, desc.height, is3D ? desc.depth : 1, desc.format, level, packedX,
                                                 packedY, packedZ);
            }
            const uint32_t pitchBlocks = guestLevel.row_pitch_bytes / guestBlockBytes;
            const uint32_t heightBlocksAligned = guestLevel.z_slice_stride_block_rows;
            if (!pitchBlocks || !levelAddress) return out;

            for (uint32_t layer = 0; layer < (is3D ? 1u : desc.layers); ++layer) {
                const GuestAddr layerAddress = levelAddress + layer * guestLevel.array_slice_stride_bytes;
                DecodedSubresource sub;
                sub.level = level;
                sub.layer = layer;
                sub.width = width;
                sub.height = height;
                sub.depth = depth;
                if (decode4x4 || yuv) {
                    sub.rowBytes = width * hostBlockBytes;
                    sub.rowCount = height;
                } else {
                    const uint32_t hostBlocksX = (width + hostBlock - 1) / hostBlock;
                    sub.rowBytes = hostBlocksX * hostBlockBytes;
                    sub.rowCount = (height + hostBlock - 1) / hostBlock;
                }
                sub.offset = (out.data.size() + 15) & ~size_t(15);
                out.data.resize(sub.offset + size_t(sub.rowBytes) * sub.rowCount * depth);
                uint8_t* dstBase = out.data.data() + sub.offset;

                blockRow.resize(size_t(blocksX) * guestBlockBytes);
                for (uint32_t z = 0; z < depth; ++z) {
                    for (uint32_t by = 0; by < blocksY; ++by) {
                        // Linha de blocos do guest, com o endian do host.
                        for (uint32_t bx = 0; bx < blocksX; ++bx) {
                            const int32_t gx = int32_t(bx + packedX), gy = int32_t(by + packedY), gz = int32_t(z + packedZ);
                            int64_t offset;
                            if (desc.tiled) {
                                offset = is3D ? texture_address::Tiled3D(gx, gy, gz, pitchBlocks, heightBlocksAligned, bpbLog2)
                                              : texture_address::Tiled2D(gx, gy, pitchBlocks, bpbLog2);
                            } else {
                                offset = (int64_t(gz) * heightBlocksAligned + gy) * guestLevel.row_pitch_bytes +
                                         int64_t(gx) * guestBlockBytes;
                            }
                            uint8_t* blockOut = blockRow.data() + size_t(bx) * guestBlockBytes;
                            const uint64_t address = uint64_t(layerAddress) + uint64_t(offset);
                            if (offset < 0 || !memory.IsReadable(GuestAddr(address & ~3ull), guestBlockBytes + 4)) {
                                std::memset(blockOut, 0, guestBlockBytes);
                                continue;
                            }
                            // Troca de endian em palavras de 32 bits alinhadas (cobre blocos de 1 e 2 bytes).
                            const GuestAddr wordStart = GuestAddr(address) & ~3u;
                            const uint32_t skip = GuestAddr(address) & 3u;
                            uint8_t words[20];
                            const uint32_t wordCount = (skip + guestBlockBytes + 3) / 4;
                            for (uint32_t w = 0; w < wordCount; ++w) {
                                uint32_t raw;
                                std::memcpy(&raw, memory.Ptr(wordStart + w * 4), 4);
                                raw = xenos::GpuSwap(raw, desc.endian);
                                std::memcpy(words + w * 4, &raw, 4);
                            }
                            std::memcpy(blockOut, words + skip, guestBlockBytes);
                        }

                        // Linha de blocos -> host.
                        if (decode4x4) {
                            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                                uint8_t texels[4 * 4 * 2];
                                DecodeBlock4x4(desc.format, blockRow.data() + bx * guestBlockBytes, texels, 4 * hostBlockBytes,
                                               hostBlockBytes);
                                for (uint32_t ty = 0; ty < 4 && by * 4 + ty < height; ++ty) {
                                    const uint32_t texelsX = std::min(4u, width - bx * 4);
                                    std::memcpy(dstBase + (size_t(z) * sub.rowCount + by * 4 + ty) * sub.rowBytes +
                                                    size_t(bx) * 4 * hostBlockBytes,
                                                texels + ty * 4 * hostBlockBytes, size_t(texelsX) * hostBlockBytes);
                                }
                            }
                            continue;
                        }
                        uint8_t* dstRow = dstBase + (size_t(z) * sub.rowCount + by) * sub.rowBytes;
                        if (yuv) {
                            std::vector<uint8_t> rgba(size_t(blocksX) * 8);
                            ConvertRow(desc.format, false, blockRow.data(), rgba.data(), blocksX);
                            std::memcpy(dstRow, rgba.data(), std::min<size_t>(rgba.size(), sub.rowBytes));
                        } else if (IsPassthrough(desc.format)) {
                            std::memcpy(dstRow, blockRow.data(), std::min<size_t>(blockRow.size(), sub.rowBytes));
                        } else {
                            ConvertRow(desc.format, desc.signedComponents, blockRow.data(), dstRow, blocksX);
                        }
                    }
                }
                out.subresources.push_back(sub);
            }
        }
        out.ok = true;
        return out;
    }

    uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
        const auto* p = static_cast<const uint8_t*>(data);
        uint64_t h = seed ^ (0x9E3779B97F4A7C15ull * (size + 1));
        size_t i = 0;
        for (; i + 8 <= size; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            h ^= v * 0xBF58476D1CE4E5B9ull;
            h = std::rotl(h, 31) * 0x94D049BB133111EBull;
        }
        for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        return h;
    }

    uint64_t HashTextureMemory(const TextureDescription& desc, const GuestMemory& memory) {
        if (!desc.valid) return 0;
        uint64_t h = 0;
        if (desc.baseAddress && memory.IsReadable(desc.baseAddress, desc.baseExtentBytes)) {
            h = HashBytes(memory.Ptr(desc.baseAddress), desc.baseExtentBytes, h);
        }
        if (desc.mipAddress && desc.mipExtentBytes && memory.IsReadable(desc.mipAddress, desc.mipExtentBytes)) {
            h = HashBytes(memory.Ptr(desc.mipAddress), desc.mipExtentBytes, h);
        }
        return h;
    }
}
