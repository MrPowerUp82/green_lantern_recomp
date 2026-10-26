#include "gpu/selftest_scene.h"

#include "recompiled/ppc_context.h"

#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/pm4.h"
#include "gpu/shader/ucode_builder.h"

#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSwap);

namespace Gpu {
    namespace {
        namespace U = Gpu::Shader::Ucode;
        namespace xenos = xe::gpu::xenos;
        using xe::gpu::ucode::AluVectorOpcode;

        constexpr uint32_t kWidth = 1280;
        constexpr uint32_t kHeight = 720;
        constexpr uint32_t kTextureSize = 64;
        constexpr uint32_t kTextureFetch = 1;   // tfetch: fetch constant de textura 1 (dwords 6..11)
        constexpr uint32_t kVertexFetch = 6;    // vfetch: fetch constant de vertice 6 (slot de textura 2)
        constexpr uint32_t kStrideDwords = 5;   // posicao float3 + uv float2
        constexpr uint32_t kClearColor = 0xFF804020u;  // k_8_8_8_8: R = bits 0..7 -> (0x20, 0x40, 0x80, 0xFF)
        constexpr uint32_t kTexelLeft = 0xFF0000FFu;   // vermelho
        constexpr uint32_t kTexelRight = 0xFFFF0000u;  // azul
        constexpr float kTint[4] = { 1.0f, 1.0f, 0.5f, 1.0f };
        constexpr float kBackColor[4] = { 0.0f, 1.0f, 0.0f, 1.0f };  // triangulo de tras, verde
        constexpr float kRectColor[4] = { 1.0f, 1.0f, 0.0f, 1.0f };  // rect list, amarelo

        // Escreve pacotes PM4 em big-endian na memoria do guest.
        struct Pm4Writer {
            const GuestMemory& mem;
            GuestAddr base;
            uint32_t count = 0;

            void Dword(uint32_t v) { mem.StoreBe32(base + 4 * count++, v); }
            void Reg(uint32_t index, uint32_t value) {
                Dword(Pm4::MakeType0(index, 1));
                Dword(value);
            }
            void RegFloat(uint32_t index, float value) { Reg(index, std::bit_cast<uint32_t>(value)); }
            void Regs(uint32_t index, std::initializer_list<uint32_t> values) {
                Dword(Pm4::MakeType0(index, uint32_t(values.size())));
                for (uint32_t v : values) Dword(v);
            }
            void Type3(uint32_t opcode, std::initializer_list<uint32_t> payload) {
                Dword(Pm4::MakeType3(opcode, uint32_t(payload.size())));
                for (uint32_t v : payload) Dword(v);
            }
            void Shader(uint32_t type, const std::vector<uint32_t>& ucode) {
                Dword(Pm4::MakeType3(Pm4::kImLoadImmediate, uint32_t(ucode.size()) + 2));
                Dword(type);
                Dword(uint32_t(ucode.size()));
                for (uint32_t v : ucode) Dword(v);
            }
        };

        std::vector<uint32_t> VertexShader() {
            std::vector<uint32_t> code;
            // r1 = posicao (float3, w = 1), r2 = uv (float2) do vertice r0.x.
            U::EmitVertexFetch(code, 1, "xyz1", 0, kVertexFetch, xenos::VertexFormat::k_32_32_32_FLOAT, kStrideDwords, 0, true);
            U::EmitVertexFetch(code, 2, "xy01", 0, kVertexFetch, xenos::VertexFormat::k_32_32_FLOAT, kStrideDwords, 3, true);
            U::EmitAlu(code, U::Mov(62, U::R(1), 0xF, true));  // oPos
            U::EmitAlu(code, U::Mov(0, U::R(2), 0xF, true));   // o0 = uv
            return U::Assemble({ U::Exec(1, 4, 0b0011, true), U::Nop() }, code);
        }

        // oC0 = c1 (cor constante).
        std::vector<uint32_t> ConstantColorPixelShader() {
            std::vector<uint32_t> code;
            U::EmitAlu(code, U::Mov(0, U::C(1), 0xF, true));
            return U::Assemble({ U::Exec(1, 1, 0, true), U::Nop() }, code);
        }

        std::vector<uint32_t> PixelShader() {
            std::vector<uint32_t> code;
            // r0 = tex2D(fc1, r0.xy); oC0 = r0 * c0.
            U::EmitTextureFetch2D(code, 0, "xyzw", 0, kTextureFetch);
            U::EmitAlu(code, U::Vector(AluVectorOpcode::kMul, 0, 0xF, U::R(0), U::C(0), {}, true));
            return U::Assemble({ U::Exec(1, 2, 0b01, true), U::Nop() }, code);
        }

        // Retangulo do resolve/clear (rect list de 3 vertices float2 na fetch constant de vertice 0).
        void WriteRectVertices(const GuestMemory& mem, GuestAddr at) {
            const float rect[6] = { 0.0f, 0.0f, float(kWidth), 0.0f, float(kWidth), float(kHeight) };
            for (int i = 0; i < 6; ++i) mem.StoreBe32(at + i * 4, std::bit_cast<uint32_t>(rect[i]));
        }
    }

    SceneResult RunDrawSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel) {
        using namespace xe::gpu;
        SceneResult result;
        GpuSystem& gpu = GpuSystem::Get();
        if (!gpu.IsInitialized()) return result;
        const GuestMemory& mem = gpu.Memory();
        uint8_t* base = mem.Base();

        constexpr uint32_t kRingSizeLog2 = 13;
        constexpr uint32_t kRingDwords = (1u << (kRingSizeLog2 + 3)) / 4;
        constexpr uint32_t kSwapDwords = 64;
        constexpr uint32_t kFrameDwords = 3 + kSwapDwords;

        const GuestAddr ring = gpu.Allocate(kRingDwords * 4);
        const GuestAddr scratch = gpu.Allocate(0x1000);
        const GuestAddr frontbuffer = gpu.Allocate(kWidth * kHeight * 4);
        const GuestAddr commands = gpu.Allocate(0x4000);
        const GuestAddr vertices = gpu.Allocate(0x1000);
        const GuestAddr rect = gpu.Allocate(0x1000);
        const GuestAddr texture = gpu.Allocate(kTextureSize * kTextureSize * 4);
        const GuestAddr backVertices = gpu.Allocate(0x1000);
        const GuestAddr depthTexture = gpu.Allocate(kWidth * kHeight * 4);
        const GuestAddr rectListVertices = gpu.Allocate(0x1000);
        if (!ring || !scratch || !frontbuffer || !commands || !vertices || !rect || !texture || !backVertices ||
            !depthTexture || !rectListVertices) {
            return result;
        }
        std::memset(base + ring, 0, kRingDwords * 4);
        std::memset(base + scratch, 0, 0x1000);

        // Triangulo em clip space (D3D: y para cima) com uv.
        const float tri[3][5] = {
            { -0.8f, -0.8f, 0.5f, 0.0f, 1.0f },
            { 0.8f, -0.8f, 0.5f, 1.0f, 1.0f },
            { 0.0f, 0.8f, 0.5f, 0.5f, 0.0f },
        };
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 5; ++c) mem.StoreBe32(vertices + (v * 5 + c) * 4, std::bit_cast<uint32_t>(tri[v][c]));
        }
        WriteRectVertices(mem, rect);
        // Triangulo de tras (z = 0.7) no canto inferior esquerdo, parcialmente atras do primeiro (z = 0.5).
        const float back[3][5] = {
            { -1.0f, -1.0f, 0.7f, 0.0f, 0.0f },
            { 0.0f, -1.0f, 0.7f, 0.0f, 0.0f },
            { -1.0f, 1.0f, 0.7f, 0.0f, 0.0f },
        };
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 5; ++c) mem.StoreBe32(backVertices + (v * 5 + c) * 4, std::bit_cast<uint32_t>(back[v][c]));
        }
        // Rect list no canto superior esquerdo (z = 0.3): tres cantos; o quarto (-0.55, 0.55) e do VS.
        const float rectList[3][5] = {
            { -0.95f, 0.95f, 0.3f, 0.0f, 0.0f },
            { -0.55f, 0.95f, 0.3f, 0.0f, 0.0f },
            { -0.95f, 0.55f, 0.3f, 0.0f, 0.0f },
        };
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 5; ++c) {
                mem.StoreBe32(rectListVertices + (v * 5 + c) * 4, std::bit_cast<uint32_t>(rectList[v][c]));
            }
        }

        // Textura 64x64 k_8_8_8_8 tiled: metade esquerda vermelha, direita azul (endian k8in32).
        for (uint32_t y = 0; y < kTextureSize; ++y) {
            for (uint32_t x = 0; x < kTextureSize; ++x) {
                const int32_t offset = texture_address::Tiled2D(int32_t(x), int32_t(y), kTextureSize, 2);
                mem.StoreBe32(texture + uint32_t(offset), x < kTextureSize / 2 ? kTexelLeft : kTexelRight);
            }
        }

        // VdSwap: scratch +0x000 write-back do RPTR, +0x010 fetch constant, +0x030 ptr do frontbuffer,
        // +0x034 largura, +0x038 altura, +0x800 pilha fake.
        const GuestAddr writeback = scratch;
        const GuestAddr fetch = scratch + 0x010;
        const GuestAddr frontbufferPtr = scratch + 0x030;
        const GuestAddr widthPtr = scratch + 0x034;
        const GuestAddr heightPtr = scratch + 0x038;
        const GuestAddr stack = scratch + 0x800;
        mem.StoreBe32(fetch + 4, frontbuffer);
        mem.StoreBe32(fetch + 8, (kWidth - 1) | ((kHeight - 1) << 13));
        mem.StoreBe32(frontbufferPtr, frontbuffer);
        mem.StoreBe32(widthPtr, kWidth);
        mem.StoreBe32(heightPtr, kHeight);
        mem.StoreBe32(stack + 0x54, widthPtr);
        mem.StoreBe32(stack + 0x5C, heightPtr);

        PPCContext ctx{};
        ctx.r1.u64 = stack;
        ctx.r3.u64 = ring;
        ctx.r4.u64 = kRingSizeLog2;
        __imp__VdInitializeRingBuffer(ctx, base);
        ctx.r3.u64 = writeback;
        ctx.r4.u64 = 6;
        __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);

        // Lista de comandos do quadro (a mesma todo quadro, num indirect buffer).
        Pm4Writer w{ mem, commands };
        // Superficie 1280 de pitch, RT0 k_8_8_8_8 no tile 0, sem profundidade.
        w.Reg(XE_GPU_REG_RB_SURFACE_INFO, kWidth);
        w.Reg(XE_GPU_REG_RB_COLOR_INFO, uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) << 16);
        // Profundidade D24S8 no tile 0x500, teste LESS com escrita.
        w.Reg(XE_GPU_REG_RB_DEPTH_INFO, 0x500);
        w.Reg(XE_GPU_REG_RB_COLOR_MASK, 0xF);
        w.Reg(XE_GPU_REG_RB_DEPTHCONTROL, 0x2u | 0x4u | (uint32_t(xenos::CompareFunction::kLess) << 4));
        w.Reg(XE_GPU_REG_RB_BLENDCONTROL0, 0x00010001u);  // ONE/ZERO/ADD em cor e alfa
        w.Reg(XE_GPU_REG_PA_SC_WINDOW_OFFSET, 0);
        w.Reg(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, 0x80000000u);
        w.Reg(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, kWidth | (kHeight << 16));
        w.Reg(XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 0);
        w.Reg(XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 8192 | (8192u << 16));
        w.Reg(XE_GPU_REG_PA_SU_SC_MODE_CNTL, 0);
        w.Reg(XE_GPU_REG_PA_SU_VTX_CNTL, 0);  // centro do pixel do D3D (meio pixel)
        w.Reg(XE_GPU_REG_PA_CL_CLIP_CNTL, 1u << 19);
        w.Reg(XE_GPU_REG_PA_CL_VTE_CNTL, 0x3Fu | (1u << 10));
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_XSCALE, kWidth / 2.0f);
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_XOFFSET, kWidth / 2.0f);
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_YSCALE, -(kHeight / 2.0f));
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_YOFFSET, kHeight / 2.0f);
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_ZSCALE, 1.0f);
        w.RegFloat(XE_GPU_REG_PA_CL_VPORT_ZOFFSET, 0.0f);
        w.Reg(XE_GPU_REG_VGT_MAX_VTX_INDX, 0xFFFFFF);
        w.Reg(XE_GPU_REG_VGT_MIN_VTX_INDX, 0);
        w.Reg(XE_GPU_REG_VGT_INDX_OFFSET, 0);
        w.Reg(XE_GPU_REG_SQ_PROGRAM_CNTL, 2u | (1u << 8));  // 3 GPRs no VS, 2 no PS
        w.Reg(XE_GPU_REG_SQ_CONTEXT_MISC, 0);

        // Clear do RT0 e da profundidade (1.0) por resolve (copy_command kNull: so limpa).
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, { 3u | rect, 2u | (6u << 2) });
        w.Reg(XE_GPU_REG_RB_COLOR_CLEAR, kClearColor);
        w.Reg(XE_GPU_REG_RB_DEPTH_CLEAR, 0xFFFFFF00u);
        w.Reg(XE_GPU_REG_RB_COPY_CONTROL, (1u << 8) | (1u << 9) | (uint32_t(xenos::CopyCommand::kNull) << 20));
        w.Reg(XE_GPU_REG_RB_MODECONTROL, uint32_t(xenos::EdramMode::kCopy));
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kRectangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });

        // Triangulo texturizado.
        w.Reg(XE_GPU_REG_RB_MODECONTROL, uint32_t(xenos::EdramMode::kColorDepth));
        w.Shader(0, VertexShader());
        w.Shader(1, PixelShader());
        // Vertex fetch constant 6: tipo vertex, endereco em dwords, endian k8in32, 15 dwords.
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + kVertexFetch * 2, { 3u | vertices, 2u | (15u << 2) });
        // Texture fetch constant 1: 2D 64x64 k_8_8_8_8 tiled, k8in32, filtro point, clamp.
        {
            xenos::xe_gpu_texture_fetch_t t{};
            t.type = xenos::FetchConstantType::kTexture;
            t.clamp_x = t.clamp_y = t.clamp_z = xenos::ClampMode::kClampToEdge;
            t.pitch = kTextureSize / 32;
            t.tiled = 1;
            t.format = xenos::TextureFormat::k_8_8_8_8;
            t.endianness = xenos::Endian::k8in32;
            t.base_address = texture >> 12;
            t.size_2d.width = kTextureSize - 1;
            t.size_2d.height = kTextureSize - 1;
            t.swizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9);  // XYZW
            t.mag_filter = t.min_filter = xenos::TextureFilter::kPoint;
            t.mip_filter = xenos::TextureFilter::kBaseMap;
            t.dimension = xenos::DataDimension::k2DOrStacked;
            w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + kTextureFetch * 6,
                   { t.dword_0, t.dword_1, t.dword_2, t.dword_3, t.dword_4, t.dword_5 });
        }
        // c0 do PS (registro 256) = tinta.
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_256_X, { std::bit_cast<uint32_t>(kTint[0]), std::bit_cast<uint32_t>(kTint[1]),
                                                     std::bit_cast<uint32_t>(kTint[2]), std::bit_cast<uint32_t>(kTint[3]) });
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kTriangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });

        // Triangulo de tras, cor constante: so aparece onde o teste de profundidade passa.
        w.Shader(1, ConstantColorPixelShader());
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + kVertexFetch * 2, { 3u | backVertices, 2u | (15u << 2) });
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_256_X + 4,
               { std::bit_cast<uint32_t>(kBackColor[0]), std::bit_cast<uint32_t>(kBackColor[1]),
                 std::bit_cast<uint32_t>(kBackColor[2]), std::bit_cast<uint32_t>(kBackColor[3]) });
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kTriangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });

        // Rect list amarela (mesmo VS e PS de cor constante).
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + kVertexFetch * 2, { 3u | rectListVertices, 2u | (15u << 2) });
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_256_X + 4,
               { std::bit_cast<uint32_t>(kRectColor[0]), std::bit_cast<uint32_t>(kRectColor[1]),
                 std::bit_cast<uint32_t>(kRectColor[2]), std::bit_cast<uint32_t>(kRectColor[3]) });
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kRectangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });

        // Resolve do RT0 para o frontbuffer (k_8_8_8_8 1280x720).
        w.Regs(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, { 3u | rect, 2u | (6u << 2) });
        w.Reg(XE_GPU_REG_RB_COPY_CONTROL, uint32_t(xenos::CopyCommand::kRaw) << 20);
        w.Reg(XE_GPU_REG_RB_COPY_DEST_BASE, frontbuffer);
        w.Reg(XE_GPU_REG_RB_COPY_DEST_PITCH, kWidth | (kHeight << 16));
        w.Reg(XE_GPU_REG_RB_COPY_DEST_INFO, (uint32_t(xenos::Endian128::k8in32)) |
                                                (uint32_t(xenos::ColorFormat::k_8_8_8_8) << 7));
        w.Reg(XE_GPU_REG_RB_MODECONTROL, uint32_t(xenos::EdramMode::kCopy));
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kRectangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });
        // Resolve da profundidade (copy_src_select = 4) para outra textura.
        w.Reg(XE_GPU_REG_RB_COPY_CONTROL, 4u | (uint32_t(xenos::CopyCommand::kRaw) << 20));
        w.Reg(XE_GPU_REG_RB_COPY_DEST_BASE, depthTexture);
        w.Type3(Pm4::kDrawIndx, { 0, uint32_t(xenos::PrimitiveType::kRectangleList) |
                                         (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16) });
        w.Reg(XE_GPU_REG_RB_MODECONTROL, uint32_t(xenos::EdramMode::kColorDepth));
        const uint32_t commandDwords = w.count;

        uint32_t writeIndex = 0;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (cancel && cancel->load()) break;
            if (writeIndex + kFrameDwords > kRingDwords) {
                // O pacote do VdSwap precisa ser contiguo: preenche o fim do ring com filler.
                while (writeIndex < kRingDwords) mem.StoreBe32(ring + 4 * writeIndex++, Pm4::MakeType2());
                writeIndex = 0;
            }
            mem.StoreBe32(ring + 4 * writeIndex, Pm4::MakeType3(Pm4::kIndirectBuffer, 2));
            mem.StoreBe32(ring + 4 * (writeIndex + 1), commands);
            mem.StoreBe32(ring + 4 * (writeIndex + 2), commandDwords);
            ctx.r3.u64 = ring + (writeIndex + 3) * 4;
            ctx.r4.u64 = fetch;
            ctx.r8.u64 = frontbufferPtr;
            __imp__VdSwap(ctx, base);
            writeIndex = (writeIndex + kFrameDwords) % kRingDwords;
            std::atomic_thread_fence(std::memory_order_release);
            mem.StoreBe32(Reg::MmioAddress(Reg::kCpRbWptr), writeIndex);
            ++result.framesSubmitted;

            // Espera o CP consumir o quadro (o indirect buffer e reaproveitado).
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (mem.LoadBe32(writeback) != writeIndex) {
                if ((cancel && cancel->load()) || std::chrono::steady_clock::now() > deadline) {
                    if (!(cancel && cancel->load())) std::cerr << "[SelfTest] CP nao consumiu o quadro a tempo." << std::endl;
                    return result;
                }
                std::this_thread::yield();
            }
            if (frameIntervalMs) std::this_thread::sleep_for(std::chrono::milliseconds(frameIntervalMs));
        }

        result.frontbuffer = frontbuffer;
        result.depthTexture = depthTexture;
        // Esperado: cor da textura * tinta nas duas metades do triangulo (linha central); verde onde
        // so o triangulo de tras cobre; o da frente na sobreposicao; clear no canto superior direito.
        result.expected[0] = { 448, 360, { 255, 0, 0, 255 } };
        result.expected[1] = { 832, 360, { 0, 0, 128, 255 } };
        result.expected[2] = { 100, 650, { 0, 255, 0, 255 } };
        result.expected[3] = { 256, 576, { 255, 0, 0, 255 } };
        result.expected[4] = { 1270, 10, { 0x20, 0x40, 0x80, 0xFF } };
        result.expected[5] = { 160, 80, { 255, 255, 0, 255 } };   // metade com os 3 vertices dados
        result.expected[6] = { 256, 144, { 255, 255, 0, 255 } };  // metade com o 4o vertice calculado
        result.expectedDepth[0] = { 256, 576, 0.5f };
        result.expectedDepth[1] = { 100, 650, 0.7f };
        result.expectedDepth[2] = { 1270, 10, 1.0f };
        result.ok = result.framesSubmitted == frames;
        return result;
    }
}
