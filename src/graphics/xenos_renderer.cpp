#include "graphics/xenos_renderer.h"

#include "gpu/shader/draw_constants.h"
#include "gpu/shader/spirv_compiler.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

namespace Graphics {
    namespace xenos = xe::gpu::xenos;
    namespace reg = xe::gpu::reg;
    using Gpu::Draw::HostTextureFormat;
    using Gpu::Shader::TranslatedShader;
    using Gpu::Shader::VertexShaderMode;
    namespace DrawBlock = Gpu::Shader::DrawBlock;
    namespace Sys = Gpu::Shader::Sys;

    namespace {
        constexpr uint64_t kUploadSize = 64ull << 20;
        // Altura dos render targets do host: cobre os 10 MB do EDRAM em qualquer pitch usado na pratica.
        constexpr uint32_t kRenderTargetHeight = 2048;
        constexpr uint32_t kGapRenderTargetBase = 0xFFFF;

        std::set<std::string> s_logged;
        void LogOnce(const std::string& message) {
            if (s_logged.insert(message).second) std::cerr << "[XenosRenderer] " << message << std::endl;
        }

        plume::RenderFormat ToPlume(HostTextureFormat f) {
            using P = plume::RenderFormat;
            switch (f) {
            case HostTextureFormat::kR8Unorm: return P::R8_UNORM;
            case HostTextureFormat::kR8G8Unorm: return P::R8G8_UNORM;
            case HostTextureFormat::kR8G8B8A8Unorm: return P::R8G8B8A8_UNORM;
            case HostTextureFormat::kR8Snorm: return P::R8_SNORM;
            case HostTextureFormat::kR8G8Snorm: return P::R8G8_SNORM;
            case HostTextureFormat::kR8G8B8A8Snorm: return P::R8G8B8A8_SNORM;
            case HostTextureFormat::kR16Unorm: return P::R16_UNORM;
            case HostTextureFormat::kR16G16Unorm: return P::R16G16_UNORM;
            case HostTextureFormat::kR16G16B16A16Unorm: return P::R16G16B16A16_UNORM;
            case HostTextureFormat::kR16Snorm: return P::R16_SNORM;
            case HostTextureFormat::kR16G16Snorm: return P::R16G16_SNORM;
            case HostTextureFormat::kR16G16B16A16Snorm: return P::R16G16B16A16_SNORM;
            case HostTextureFormat::kR16Float: return P::R16_FLOAT;
            case HostTextureFormat::kR16G16Float: return P::R16G16_FLOAT;
            case HostTextureFormat::kR16G16B16A16Float: return P::R16G16B16A16_FLOAT;
            case HostTextureFormat::kR32Float: return P::R32_FLOAT;
            case HostTextureFormat::kR32G32Float: return P::R32G32_FLOAT;
            case HostTextureFormat::kR32G32B32A32Float: return P::R32G32B32A32_FLOAT;
            case HostTextureFormat::kBC1: return P::BC1_UNORM;
            case HostTextureFormat::kBC2: return P::BC2_UNORM;
            case HostTextureFormat::kBC3: return P::BC3_UNORM;
            case HostTextureFormat::kBC4: return P::BC4_UNORM;
            case HostTextureFormat::kBC5: return P::BC5_UNORM;
            case HostTextureFormat::kBC5Snorm: return P::BC5_SNORM;
            default: return P::UNKNOWN;
            }
        }

        plume::RenderFormat ColorTargetFormat(xenos::ColorRenderTargetFormat f) {
            using P = plume::RenderFormat;
            switch (f) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
                return P::R8G8B8A8_UNORM;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
                return P::R16G16B16A16_UNORM;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
                return P::R16G16_FLOAT;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT:
                return P::R32_FLOAT;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
                return P::R32G32_FLOAT;
            default:  // k_2_10_10_10_FLOAT*, k_16_16_16_16*
                return P::R16G16B16A16_FLOAT;
            }
        }

        bool FormatBlendable(plume::RenderFormat f) {
            return f != plume::RenderFormat::R32_FLOAT && f != plume::RenderFormat::R32G32_FLOAT;
        }

        plume::RenderComparisonFunction ToPlume(xenos::CompareFunction f) {
            using C = plume::RenderComparisonFunction;
            switch (f) {
            case xenos::CompareFunction::kNever: return C::NEVER;
            case xenos::CompareFunction::kLess: return C::LESS;
            case xenos::CompareFunction::kEqual: return C::EQUAL;
            case xenos::CompareFunction::kLessEqual: return C::LESS_EQUAL;
            case xenos::CompareFunction::kGreater: return C::GREATER;
            case xenos::CompareFunction::kNotEqual: return C::NOT_EQUAL;
            case xenos::CompareFunction::kGreaterEqual: return C::GREATER_EQUAL;
            default: return C::ALWAYS;
            }
        }

        plume::RenderStencilOp ToPlume(xenos::StencilOp op) {
            using S = plume::RenderStencilOp;
            switch (op) {
            case xenos::StencilOp::kZero: return S::ZERO;
            case xenos::StencilOp::kReplace: return S::REPLACE;
            case xenos::StencilOp::kIncrementClamp: return S::INCREMENT_AND_CLAMP;
            case xenos::StencilOp::kDecrementClamp: return S::DECREMENT_AND_CLAMP;
            case xenos::StencilOp::kInvert: return S::INVERT;
            case xenos::StencilOp::kIncrementWrap: return S::INCREMENT_AND_WRAP;
            case xenos::StencilOp::kDecrementWrap: return S::DECREMENT_AND_WRAP;
            default: return S::KEEP;
            }
        }

        plume::RenderBlend ToPlume(xenos::BlendFactor f) {
            using B = plume::RenderBlend;
            switch (f) {
            case xenos::BlendFactor::kZero: return B::ZERO;
            case xenos::BlendFactor::kOne: return B::ONE;
            case xenos::BlendFactor::kSrcColor: return B::SRC_COLOR;
            case xenos::BlendFactor::kOneMinusSrcColor: return B::INV_SRC_COLOR;
            case xenos::BlendFactor::kSrcAlpha: return B::SRC_ALPHA;
            case xenos::BlendFactor::kOneMinusSrcAlpha: return B::INV_SRC_ALPHA;
            case xenos::BlendFactor::kDstColor: return B::DEST_COLOR;
            case xenos::BlendFactor::kOneMinusDstColor: return B::INV_DEST_COLOR;
            case xenos::BlendFactor::kDstAlpha: return B::DEST_ALPHA;
            case xenos::BlendFactor::kOneMinusDstAlpha: return B::INV_DEST_ALPHA;
            // As constantes de blend nao sao dinamicas no Plume (ficam zero).
            case xenos::BlendFactor::kConstantColor:
            case xenos::BlendFactor::kConstantAlpha: return B::BLEND_FACTOR;
            case xenos::BlendFactor::kOneMinusConstantColor:
            case xenos::BlendFactor::kOneMinusConstantAlpha: return B::INV_BLEND_FACTOR;
            case xenos::BlendFactor::kSrcAlphaSaturate: return B::SRC_ALPHA_SAT;
            default: return B::ONE;
            }
        }

        plume::RenderBlendOperation ToPlume(xenos::BlendOp op) {
            using O = plume::RenderBlendOperation;
            switch (op) {
            case xenos::BlendOp::kSubtract: return O::SUBTRACT;
            case xenos::BlendOp::kMin: return O::MIN;
            case xenos::BlendOp::kMax: return O::MAX;
            case xenos::BlendOp::kRevSubtract: return O::REV_SUBTRACT;
            default: return O::ADD;
            }
        }

        plume::RenderPrimitiveTopology ToPlume(Gpu::Draw::HostTopology t) {
            using T = plume::RenderPrimitiveTopology;
            switch (t) {
            case Gpu::Draw::HostTopology::kPointList: return T::POINT_LIST;
            case Gpu::Draw::HostTopology::kLineList: return T::LINE_LIST;
            case Gpu::Draw::HostTopology::kLineStrip: return T::LINE_STRIP;
            case Gpu::Draw::HostTopology::kTriangleStrip: return T::TRIANGLE_STRIP;
            case Gpu::Draw::HostTopology::kTriangleFan: return T::TRIANGLE_FAN;
            default: return T::TRIANGLE_LIST;
            }
        }

        plume::RenderTextureAddressMode ToPlume(xenos::ClampMode m) {
            using A = plume::RenderTextureAddressMode;
            switch (m) {
            case xenos::ClampMode::kRepeat: return A::WRAP;
            case xenos::ClampMode::kMirroredRepeat: return A::MIRROR;
            case xenos::ClampMode::kMirrorClampToEdge:
            case xenos::ClampMode::kMirrorClampToHalfway: return A::MIRROR_ONCE;
            case xenos::ClampMode::kClampToBorder:
            case xenos::ClampMode::kMirrorClampToBorder: return A::BORDER;
            default: return A::CLAMP;
            }
        }

        void Append(std::string& key, const void* data, size_t size) {
            key.append(static_cast<const char*>(data), size);
        }
        template <typename T>
        void Append(std::string& key, const T& value) {
            Append(key, &value, sizeof(value));
        }

        const char* kPresentVertexGlsl = R"(#version 450
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
        // Resolve de profundidade: le o depth (aspecto de profundidade) no mesmo pixel e grava em R32F.
        const char* kDepthCopyPixelGlsl = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D depthTexture;
layout(set = 0, binding = 1) uniform sampler depthSampler;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(texelFetch(sampler2D(depthTexture, depthSampler), ivec2(gl_FragCoord.xy), 0).r);
}
)";
        const char* kPresentPixelGlsl = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D frontbuffer;
layout(set = 0, binding = 1) uniform sampler frontbufferSampler;
layout(push_constant) uniform Present { vec4 uvScale; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(texture(sampler2D(frontbuffer, frontbufferSampler), uv * pc.uvScale.xy).rgb, 1.0);
}
)";
    }

    XenosRenderer::XenosRenderer(plume::RenderDevice* device, plume::RenderCommandQueue* queue, uint8_t* guestBase)
        : m_device(device), m_queue(queue), m_memory(guestBase) {}

    XenosRenderer::~XenosRenderer() {
        if (m_recording) {
            m_commandList->end();
            m_recording = false;
        }
        m_frameSets.clear();
    }

    bool XenosRenderer::Initialize() {
        m_commandList = m_queue->createCommandList();
        m_fence = m_device->createCommandFence();
        m_upload = m_device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
            kUploadSize, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::INDEX));
        if (!m_commandList || !m_fence || !m_upload) return false;
        m_uploadMapped = static_cast<uint8_t*>(m_upload->map());
        m_uploadSize = kUploadSize;
        if (!m_uploadMapped) return false;

        m_ringRange = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::STRUCTURED_BUFFER, 0, 1);
        m_ringSetDesc = std::make_unique<plume::RenderDescriptorSetDesc>(&m_ringRange, 1);
        m_ringSet = m_device->createDescriptorSet(*m_ringSetDesc);
        m_ringSet->setBuffer(0, m_upload.get(), m_uploadSize);

        // Texturas vazias (pretas) para bindings sem textura valida.
        const uint32_t black = 0;
        auto makeDummy = [&](plume::RenderTextureDesc desc, plume::RenderTextureViewDesc viewDesc,
                             std::unique_ptr<plume::RenderTexture>& texture, std::unique_ptr<plume::RenderTextureView>& view) {
            texture = m_device->createTexture(desc);
            if (!texture) return false;
            view = texture->createTextureView(viewDesc);
            return view != nullptr;
        };
        bool ok = makeDummy(plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM),
                            plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM), m_dummy2D,
                            m_dummy2DView);
        plume::RenderTextureDesc arrayDesc = plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM);
        arrayDesc.arraySize = 2;
        ok = ok && makeDummy(arrayDesc, plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM),
                             m_dummy2DArray, m_dummy2DArrayView);
        ok = ok && makeDummy(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM),
                             plume::RenderTextureViewDesc::Texture3D(plume::RenderFormat::R8G8B8A8_UNORM), m_dummy3D,
                             m_dummy3DView);
        plume::RenderTextureDesc cubeDesc =
            plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM, plume::RenderTextureFlag::CUBE);
        cubeDesc.arraySize = 6;
        ok = ok && makeDummy(cubeDesc, plume::RenderTextureViewDesc::TextureCube(plume::RenderFormat::R8G8B8A8_UNORM),
                             m_dummyCube, m_dummyCubeView);
        if (!ok) return false;
        m_defaultSampler = m_device->createSampler(plume::RenderSamplerDesc());

        // Preenche as texturas vazias com preto e as deixa em SHADER_READ.
        EnsureRecording();
        std::vector<uint32_t> zeros(8, black);
        const uint64_t offset = Upload(zeros.data(), zeros.size() * 4, 16);
        for (plume::RenderTexture* t : { m_dummy2D.get(), m_dummy2DArray.get(), m_dummy3D.get(), m_dummyCube.get() }) {
            m_commandList->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(t, plume::RenderTextureLayout::COPY_DEST));
            const uint32_t layers = t == m_dummyCube.get() ? 6 : (t == m_dummy2DArray.get() ? 2 : 1);
            for (uint32_t layer = 0; layer < layers; ++layer) {
                m_commandList->copyTextureRegion(
                    plume::RenderTextureCopyLocation::Subresource(t, 0, layer),
                    plume::RenderTextureCopyLocation::PlacedFootprint(m_upload.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1,
                                                                      1, 1, offset));
            }
            m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                    plume::RenderTextureBarrier(t, plume::RenderTextureLayout::SHADER_READ));
        }
        SubmitAndWait();
        return InitializePresent();
    }

    bool XenosRenderer::InitializePresent() {
        std::vector<uint32_t> spirv;
        std::string log;
        if (!Gpu::Shader::CompileGlslToSpirv(Gpu::Shader::Stage::kVertex, kPresentVertexGlsl, spirv, log)) {
            std::cerr << "[XenosRenderer] VS de apresentacao: " << log << std::endl;
            return false;
        }
        m_presentVs = m_device->createShader(spirv.data(), spirv.size() * 4, "main", plume::RenderShaderFormat::SPIRV);
        if (!Gpu::Shader::CompileGlslToSpirv(Gpu::Shader::Stage::kPixel, kPresentPixelGlsl, spirv, log)) {
            std::cerr << "[XenosRenderer] PS de apresentacao: " << log << std::endl;
            return false;
        }
        m_presentPs = m_device->createShader(spirv.data(), spirv.size() * 4, "main", plume::RenderShaderFormat::SPIRV);

        m_presentRanges = { plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
                            plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1) };
        m_presentSetDesc = std::make_unique<plume::RenderDescriptorSetDesc>(m_presentRanges.data(), uint32_t(m_presentRanges.size()));
        plume::RenderPushConstantRange push(0, 0, 0, 16, plume::RenderShaderStageFlag::PIXEL);
        m_presentLayout = m_device->createPipelineLayout(plume::RenderPipelineLayoutDesc(&push, 1, m_presentSetDesc.get(), 1));

        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = m_presentLayout.get();
        desc.vertexShader = m_presentVs.get();
        desc.pixelShader = m_presentPs.get();
        desc.depthClipEnabled = true;
        desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
        desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
        m_presentPipeline = m_device->createGraphicsPipeline(desc);

        plume::RenderSamplerDesc samplerDesc;
        samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = plume::RenderTextureAddressMode::CLAMP;
        m_presentSampler = m_device->createSampler(samplerDesc);

        // Copia de profundidade: mesmo VS e layout da apresentacao, saida R32F.
        if (!Gpu::Shader::CompileGlslToSpirv(Gpu::Shader::Stage::kPixel, kDepthCopyPixelGlsl, spirv, log)) {
            std::cerr << "[XenosRenderer] PS de copia de profundidade: " << log << std::endl;
            return false;
        }
        m_depthCopyPs = m_device->createShader(spirv.data(), spirv.size() * 4, "main", plume::RenderShaderFormat::SPIRV);
        plume::RenderGraphicsPipelineDesc depthDesc = desc;
        depthDesc.pixelShader = m_depthCopyPs.get();
        depthDesc.renderTargetFormat[0] = plume::RenderFormat::R32_FLOAT;
        m_depthCopyPipeline = m_device->createGraphicsPipeline(depthDesc);
        return m_presentVs && m_presentPs && m_presentLayout && m_presentPipeline && m_presentSampler && m_depthCopyPs &&
               m_depthCopyPipeline;
    }

    // ------------------------------------------------------------------ quadro e upload

    bool XenosRenderer::EnsureRecording() {
        if (!m_recording) {
            m_commandList->begin();
            m_recording = true;
            m_currentFramebuffer = nullptr;
        }
        return true;
    }

    void XenosRenderer::SubmitAndWait() {
        if (!m_recording) return;
        m_commandList->end();
        m_recording = false;
        const plume::RenderCommandList* list = m_commandList.get();
        m_queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, m_fence.get());
        m_queue->waitForCommandFence(m_fence.get());
        EndFrame();
    }

    void XenosRenderer::Flush() { SubmitAndWait(); }

    void XenosRenderer::EndFrame() {
        m_uploadCursor = 0;
        m_lastBlockOffset = ~0ull;
        m_frameSetCache.clear();
        m_frameSets.clear();
        m_retiredTextures.clear();
        m_retiredResolved.clear();
        m_currentFramebuffer = nullptr;
        ++m_frame;
    }

    void* XenosRenderer::UploadReserve(size_t size, size_t alignment, uint64_t& offset) {
        uint64_t aligned = (m_uploadCursor + alignment - 1) & ~uint64_t(alignment - 1);
        if (aligned + size > m_uploadSize) {
            if (size > m_uploadSize) return nullptr;
            // Buffer cheio: espera a GPU terminar o que ja foi gravado e recomeca do inicio.
            SubmitAndWait();
            EnsureRecording();
            aligned = 0;
        }
        offset = aligned;
        m_uploadCursor = aligned + size;
        return m_uploadMapped + aligned;
    }

    uint64_t XenosRenderer::Upload(const void* data, size_t size, size_t alignment) {
        uint64_t offset = 0;
        void* dst = UploadReserve(size, alignment, offset);
        if (dst && size) std::memcpy(dst, data, size);
        return offset;
    }

    // ------------------------------------------------------------------ shaders e pipelines

    XenosRenderer::ShaderEntry* XenosRenderer::GetShader(const Gpu::ShaderBlob& blob, VertexShaderMode mode) {
        const uint64_t key = blob.hash ^ (uint64_t(mode) * 0x9E3779B97F4A7C15ull);
        auto it = m_shaders.find(key);
        if (it != m_shaders.end()) return it->second->ok ? it->second.get() : nullptr;

        auto entry = std::make_unique<ShaderEntry>();
        entry->translated = Gpu::Shader::TranslateShader(blob.ucode.data(), uint32_t(blob.ucode.size()), blob.type, mode);
        if (entry->translated.ok) {
            std::vector<uint32_t> spirv;
            std::string log;
            const auto stage = blob.type == xenos::ShaderType::kVertex ? Gpu::Shader::Stage::kVertex : Gpu::Shader::Stage::kPixel;
            if (Gpu::Shader::CompileGlslToSpirv(stage, entry->translated.glsl, spirv, log)) {
                entry->shader = m_device->createShader(spirv.data(), spirv.size() * 4, "main", plume::RenderShaderFormat::SPIRV);
                entry->ok = entry->shader != nullptr;
            } else {
                LogOnce("falha ao compilar shader " + std::to_string(blob.hash) + ": " + log.substr(0, 400));
            }
        } else {
            LogOnce("falha ao traduzir shader: " + entry->translated.error);
        }
        // Assinatura das bindings de textura (dimensao de cada uma).
        for (const auto& t : entry->translated.textures) entry->layoutKey += char('0' + int(t.dimension));
        if (entry->ok) {
            ++m_stats.shadersCompiled;
            if (entry->translated.usesMemexport) LogOnce("shader com memexport (ignorado)");
        } else {
            ++m_stats.shaderFailures;
        }
        ShaderEntry* result = entry->ok ? entry.get() : nullptr;
        m_shaders.emplace(key, std::move(entry));
        return result;
    }

    XenosRenderer::LayoutEntry* XenosRenderer::GetLayout(const ShaderEntry& vs, const ShaderEntry* ps) {
        const std::string key = vs.layoutKey + "|" + (ps ? ps->layoutKey : std::string());
        auto it = m_layouts.find(key);
        if (it != m_layouts.end()) return it->second.get();

        auto entry = std::make_unique<LayoutEntry>();
        auto addRanges = [](std::vector<plume::RenderDescriptorRange>& ranges, const TranslatedShader& shader, uint32_t base) {
            for (uint32_t i = 0; i < shader.textures.size(); ++i) {
                ranges.emplace_back(plume::RenderDescriptorRangeType::TEXTURE, base + i * 3, 1);
                ranges.emplace_back(plume::RenderDescriptorRangeType::SAMPLER, base + i * 3 + 1, 1);
                if (shader.textures[i].dimension == xenos::FetchOpDimension::k3DOrStacked) {
                    ranges.emplace_back(plume::RenderDescriptorRangeType::TEXTURE, base + i * 3 + 2, 1);
                }
            }
        };
        addRanges(entry->vsRanges, vs.translated, 0);
        if (ps) addRanges(entry->psRanges, ps->translated, Gpu::Shader::kPixelTextureBindingBase);

        std::vector<plume::RenderDescriptorRange> all = entry->vsRanges;
        all.insert(all.end(), entry->psRanges.begin(), entry->psRanges.end());
        plume::RenderDescriptorSetDesc sets[2] = { *m_ringSetDesc, plume::RenderDescriptorSetDesc(all.data(), uint32_t(all.size())) };
        plume::RenderPushConstantRange push(0, 0, 0, 4, plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
        entry->layout = m_device->createPipelineLayout(plume::RenderPipelineLayoutDesc(&push, 1, sets, all.empty() ? 1 : 2));
        if (!entry->layout) return nullptr;
        LayoutEntry* result = entry.get();
        m_layouts.emplace(key, std::move(entry));
        return result;
    }

    plume::RenderPipeline* XenosRenderer::GetPipeline(const ShaderEntry& vs, const ShaderEntry* ps, const LayoutEntry& layout,
                                                      const Gpu::Draw::RenderState& state,
                                                      const Gpu::Draw::HostPrimitive& primitive,
                                                      plume::RenderFormat colorFormats[4], uint32_t colorCount,
                                                      plume::RenderFormat depthFormat) {
        plume::RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.layout.get();
        desc.vertexShader = vs.shader.get();
        desc.pixelShader = ps ? ps->shader.get() : nullptr;
        desc.primitiveTopology = ToPlume(primitive.topology);
        desc.depthClipEnabled = true;
        desc.dynamicDepthBiasEnabled = true;
        desc.cullMode = state.cull == Gpu::Draw::CullMode::kFront  ? plume::RenderCullMode::FRONT
                        : state.cull == Gpu::Draw::CullMode::kBack ? plume::RenderCullMode::BACK
                                                                   : plume::RenderCullMode::NONE;
        desc.frontFace = state.frontFaceClockwise ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;

        if (depthFormat != plume::RenderFormat::UNKNOWN) {
            desc.depthTargetFormat = depthFormat;
            desc.depthEnabled = state.depthTest;
            desc.depthWriteEnabled = state.depthWrite;
            desc.depthFunction = ToPlume(state.depthFunc);
            if (state.stencilTest) {
                const auto& dc = state.depthControl;
                desc.stencilEnabled = true;
                desc.stencilReadMask = state.stencilFront.stencilmask;
                desc.stencilWriteMask = state.stencilFront.stencilwritemask;
                desc.stencilReference = state.stencilFront.stencilref;
                desc.stencilFrontFace.compareFunction = ToPlume(dc.stencilfunc);
                desc.stencilFrontFace.failOp = ToPlume(dc.stencilfail);
                desc.stencilFrontFace.passOp = ToPlume(dc.stencilzpass);
                desc.stencilFrontFace.depthFailOp = ToPlume(dc.stencilzfail);
                if (dc.backface_enable) {
                    desc.stencilBackFace.compareFunction = ToPlume(dc.stencilfunc_bf);
                    desc.stencilBackFace.failOp = ToPlume(dc.stencilfail_bf);
                    desc.stencilBackFace.passOp = ToPlume(dc.stencilzpass_bf);
                    desc.stencilBackFace.depthFailOp = ToPlume(dc.stencilzfail_bf);
                } else {
                    desc.stencilBackFace = desc.stencilFrontFace;
                }
            }
        }

        desc.renderTargetCount = colorCount;
        for (uint32_t i = 0; i < colorCount; ++i) {
            desc.renderTargetFormat[i] = colorFormats[i];
            const Gpu::Draw::ColorTarget& rt = state.color[i];
            plume::RenderBlendDesc& b = desc.renderTargetBlend[i];
            const bool used = rt.enabled && ps && (ps->translated.colorTargetsWritten & (1u << i));
            b.renderTargetWriteMask = used ? uint8_t(rt.writeMask) : 0;
            const reg::RB_BLENDCONTROL bc = rt.blend;
            b.srcBlend = ToPlume(bc.color_srcblend);
            b.dstBlend = ToPlume(bc.color_destblend);
            b.blendOp = ToPlume(bc.color_comb_fcn);
            b.srcBlendAlpha = ToPlume(bc.alpha_srcblend);
            b.dstBlendAlpha = ToPlume(bc.alpha_destblend);
            b.blendOpAlpha = ToPlume(bc.alpha_comb_fcn);
            const bool opaque = bc.color_srcblend == xenos::BlendFactor::kOne && bc.color_destblend == xenos::BlendFactor::kZero &&
                                bc.color_comb_fcn == xenos::BlendOp::kAdd && bc.alpha_srcblend == xenos::BlendFactor::kOne &&
                                bc.alpha_destblend == xenos::BlendFactor::kZero && bc.alpha_comb_fcn == xenos::BlendOp::kAdd;
            b.blendEnabled = used && !opaque && FormatBlendable(colorFormats[i]);
        }

        // Chave: tudo que entra no desc (os ponteiros dos shaders identificam o programa).
        std::string key;
        Append(key, desc.vertexShader);
        Append(key, desc.pixelShader);
        Append(key, desc.pipelineLayout);
        Append(key, desc.primitiveTopology);
        Append(key, desc.cullMode);
        Append(key, desc.frontFace);
        Append(key, desc.depthTargetFormat);
        Append(key, desc.depthEnabled);
        Append(key, desc.depthWriteEnabled);
        Append(key, desc.depthFunction);
        Append(key, desc.stencilEnabled);
        if (desc.stencilEnabled) {
            Append(key, desc.stencilReadMask);
            Append(key, desc.stencilWriteMask);
            Append(key, desc.stencilReference);
            Append(key, desc.stencilFrontFace);
            Append(key, desc.stencilBackFace);
        }
        Append(key, colorCount);
        for (uint32_t i = 0; i < colorCount; ++i) {
            Append(key, desc.renderTargetFormat[i]);
            Append(key, desc.renderTargetBlend[i]);
        }
        auto it = m_pipelines.find(key);
        if (it != m_pipelines.end()) return it->second.get();
        auto pipeline = m_device->createGraphicsPipeline(desc);
        plume::RenderPipeline* result = pipeline.get();
        if (pipeline) ++m_stats.pipelinesCreated;
        m_pipelines.emplace(std::move(key), std::move(pipeline));
        return result;
    }

    // ------------------------------------------------------------------ render targets

    XenosRenderer::RenderTarget* XenosRenderer::GetRenderTarget(uint32_t edramBase, uint32_t pitch, plume::RenderFormat format,
                                                                bool isDepth) {
        pitch = std::clamp(pitch, 16u, 8192u);
        const auto key = std::make_tuple(edramBase, pitch, int(format), isDepth);
        auto it = m_renderTargets.find(key);
        if (it != m_renderTargets.end()) return it->second.get();

        auto rt = std::make_unique<RenderTarget>();
        rt->format = format;
        rt->width = pitch;
        rt->height = kRenderTargetHeight;
        rt->isDepth = isDepth;
        rt->texture = isDepth ? m_device->createTexture(plume::RenderTextureDesc::DepthTarget(rt->width, rt->height, format))
                              : m_device->createTexture(plume::RenderTextureDesc::ColorTarget(rt->width, rt->height, format));
        if (!rt->texture) return nullptr;
        // Conteudo inicial definido (o EDRAM do guest tambem comeca com lixo, mas evitamos lixo do host).
        EnsureRecording();
        const plume::RenderTexture* attachment = rt->texture.get();
        plume::RenderFramebufferDesc fbDesc;
        if (isDepth) {
            fbDesc.depthAttachment = attachment;
        } else {
            fbDesc.colorAttachments = &attachment;
            fbDesc.colorAttachmentsCount = 1;
        }
        auto framebuffer = m_device->createFramebuffer(fbDesc);
        Transition(rt.get(), isDepth ? plume::RenderTextureLayout::DEPTH_WRITE : plume::RenderTextureLayout::COLOR_WRITE);
        m_commandList->setFramebuffer(framebuffer.get());
        if (isDepth) {
            m_commandList->clearDepthStencil(true, true, 1.0f, 0);
        } else {
            m_commandList->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        }
        m_commandList->setFramebuffer(nullptr);
        m_currentFramebuffer = nullptr;
        // O framebuffer temporario precisa viver ate a execucao: guarda no cache.
        m_framebuffers.emplace(std::vector<const void*>{ attachment, nullptr, reinterpret_cast<const void*>(1) },
                               std::move(framebuffer));
        RenderTarget* result = rt.get();
        m_renderTargets.emplace(key, std::move(rt));
        return result;
    }

    plume::RenderFramebuffer* XenosRenderer::GetFramebuffer(RenderTarget* const* colors, uint32_t colorCount, RenderTarget* depth) {
        std::vector<const void*> key;
        for (uint32_t i = 0; i < colorCount; ++i) key.push_back(colors[i]->texture.get());
        key.push_back(depth ? depth->texture.get() : nullptr);
        auto it = m_framebuffers.find(key);
        if (it != m_framebuffers.end()) return it->second.get();
        std::vector<const plume::RenderTexture*> attachments;
        for (uint32_t i = 0; i < colorCount; ++i) attachments.push_back(colors[i]->texture.get());
        plume::RenderFramebufferDesc desc;
        desc.colorAttachments = attachments.empty() ? nullptr : attachments.data();
        desc.colorAttachmentsCount = colorCount;
        desc.depthAttachment = depth ? depth->texture.get() : nullptr;
        auto framebuffer = m_device->createFramebuffer(desc);
        plume::RenderFramebuffer* result = framebuffer.get();
        m_framebuffers.emplace(std::move(key), std::move(framebuffer));
        return result;
    }

    // ------------------------------------------------------------------ texturas

    XenosRenderer::BoundTexture XenosRenderer::GetTexture(uint32_t fetchConstant, const Gpu::RegisterFileView& regs) {
        BoundTexture bound;
        const xenos::xe_gpu_texture_fetch_t fetch = regs.TextureFetch(fetchConstant);
        const Gpu::Draw::TextureDescription desc = Gpu::Draw::DescribeTexture(fetch);
        if (!desc.valid) return bound;

        // Resultado de um resolve no mesmo endereco: usa a copia do render target.
        if (auto resolved = m_resolved.find(desc.baseAddress); resolved != m_resolved.end()) {
            bound.texture = resolved->second->texture.get();
            bound.view = resolved->second->view.get();
            return bound;
        }

        const uint64_t key = Gpu::Draw::HashBytes(&desc, sizeof(desc));
        CachedTexture* entry;
        auto it = m_textures.find(key);
        if (it == m_textures.end()) {
            auto created = std::make_unique<CachedTexture>();
            created->desc = desc;
            entry = created.get();
            m_textures.emplace(key, std::move(created));
        } else {
            entry = it->second.get();
        }

        if (entry->validatedFrame != m_frame || !entry->texture) {
            const uint64_t hash = Gpu::Draw::HashTextureMemory(desc, m_memory);
            entry->validatedFrame = m_frame;
            if (!entry->texture || hash != entry->contentHash) {
                entry->contentHash = hash;
                const Gpu::Draw::DecodedTexture decoded = Gpu::Draw::DecodeTexture(desc, m_memory);
                const plume::RenderFormat format = ToPlume(decoded.format);
                if (!decoded.ok || format == plume::RenderFormat::UNKNOWN) {
                    LogOnce("formato de textura sem suporte: " + std::to_string(uint32_t(desc.format)));
                    if (entry->texture) {
                        // Pode estar em comandos ja gravados neste quadro.
                        auto retired = std::make_unique<CachedTexture>();
                        retired->texture = std::move(entry->texture);
                        retired->view = std::move(entry->view);
                        retired->arrayView = std::move(entry->arrayView);
                        m_retiredTextures.push_back(std::move(retired));
                    }
                    return bound;
                }
                if (!entry->texture) {
                    plume::RenderTextureDesc td;
                    switch (desc.dimension) {
                    case xenos::DataDimension::k3D:
                        td = plume::RenderTextureDesc::Texture3D(desc.width, desc.height, desc.depth, desc.mipLevels, format);
                        break;
                    case xenos::DataDimension::kCube:
                        td = plume::RenderTextureDesc::Texture2D(desc.width, desc.height, desc.mipLevels, format,
                                                                 plume::RenderTextureFlag::CUBE);
                        td.arraySize = 6;
                        break;
                    default:
                        td = plume::RenderTextureDesc::Texture2D(desc.width, desc.height, desc.mipLevels, format);
                        td.arraySize = desc.layers;
                        break;
                    }
                    entry->texture = m_device->createTexture(td);
                    if (!entry->texture) return bound;
                    plume::RenderTextureViewDesc vd;
                    if (desc.dimension == xenos::DataDimension::k3D) {
                        vd = plume::RenderTextureViewDesc::Texture3D(format);
                    } else if (desc.dimension == xenos::DataDimension::kCube) {
                        vd = plume::RenderTextureViewDesc::TextureCube(format);
                    } else {
                        vd = plume::RenderTextureViewDesc::Texture2D(format);
                        vd.arraySize = 1;
                        if (desc.layers > 1) {
                            plume::RenderTextureViewDesc avd = plume::RenderTextureViewDesc::Texture2D(format);
                            entry->arrayView = entry->texture->createTextureView(avd);
                        }
                    }
                    entry->view = entry->texture->createTextureView(vd);
                }
                // Upload: todos os subresources pelo buffer de upload.
                uint64_t base = 0;
                void* dst = UploadReserve(decoded.data.size(), 16, base);
                if (!dst) return bound;
                std::memcpy(dst, decoded.data.data(), decoded.data.size());
                EnsureRecording();
                m_commandList->barriers(plume::RenderBarrierStage::COPY,
                                        plume::RenderTextureBarrier(entry->texture.get(), plume::RenderTextureLayout::COPY_DEST));
                const uint32_t block = Gpu::Draw::HostFormatBlockSize(decoded.format);
                const uint32_t blockBytes = Gpu::Draw::HostFormatBytesPerBlock(decoded.format);
                for (const Gpu::Draw::DecodedSubresource& sub : decoded.subresources) {
                    const uint32_t rowTexels = sub.rowBytes / blockBytes * block;
                    m_commandList->copyTextureRegion(
                        plume::RenderTextureCopyLocation::Subresource(entry->texture.get(), sub.level, sub.layer),
                        plume::RenderTextureCopyLocation::PlacedFootprint(m_upload.get(), format, sub.width, sub.height, sub.depth,
                                                                          rowTexels, base + sub.offset));
                }
                m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                        plume::RenderTextureBarrier(entry->texture.get(), plume::RenderTextureLayout::SHADER_READ));
                ++m_stats.texturesUploaded;
            }
        }
        if (!entry->texture) return bound;
        bound.texture = entry->texture.get();
        bound.view = entry->view.get();
        if (entry->arrayView) {
            bound.arrayTexture = entry->texture.get();
            bound.arrayView = entry->arrayView.get();
        }
        return bound;
    }

    plume::RenderSampler* XenosRenderer::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                    const Gpu::Shader::TextureBinding& binding) {
        auto pick = [](xenos::TextureFilter instr, xenos::TextureFilter fetchValue) {
            return instr == xenos::TextureFilter::kUseFetchConst ? fetchValue : instr;
        };
        const xenos::TextureFilter mag = pick(binding.magFilter, fetch.mag_filter);
        const xenos::TextureFilter min = pick(binding.minFilter, fetch.min_filter);
        const xenos::TextureFilter mip = pick(binding.mipFilter, fetch.mip_filter);
        const xenos::AnisoFilter aniso =
            binding.anisoFilter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.anisoFilter;

        plume::RenderSamplerDesc desc;
        desc.magFilter = mag == xenos::TextureFilter::kLinear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        desc.minFilter = min == xenos::TextureFilter::kLinear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        desc.mipmapMode = mip == xenos::TextureFilter::kLinear ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
        desc.addressU = ToPlume(fetch.clamp_x);
        desc.addressV = ToPlume(fetch.clamp_y);
        desc.addressW = ToPlume(fetch.clamp_z);
        if (aniso != xenos::AnisoFilter::kDisabled && aniso != xenos::AnisoFilter::kUseFetchConst) {
            desc.anisotropyEnabled = true;
            desc.maxAnisotropy = 1u << (uint32_t(aniso) - 1);
            desc.minFilter = desc.magFilter = plume::RenderFilter::LINEAR;
            desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
        }
        desc.minLOD = float(fetch.mip_min_level);
        desc.maxLOD = mip == xenos::TextureFilter::kBaseMap ? desc.minLOD : float(fetch.mip_max_level);
        desc.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White ? plume::RenderBorderColor::OPAQUE_WHITE
                           : fetch.border_color == xenos::BorderColor::k_ABGR_Black ? plume::RenderBorderColor::TRANSPARENT_BLACK
                                                                                    : plume::RenderBorderColor::OPAQUE_BLACK;

        const uint64_t key = Gpu::Draw::HashBytes(&desc, sizeof(desc));
        auto it = m_samplers.find(key);
        if (it != m_samplers.end()) return it->second.get();
        auto sampler = m_device->createSampler(desc);
        plume::RenderSampler* result = sampler.get();
        m_samplers.emplace(key, std::move(sampler));
        return result;
    }

    bool XenosRenderer::CopyDepth(RenderTarget* source, ResolvedTexture& dest, const plume::RenderRect& rect) {
        if (!source->depthView) {
            source->depthView =
                source->texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::D32_FLOAT_S8_UINT));
            if (!source->depthView) return false;
        }
        if (!dest.framebuffer) {
            const plume::RenderTexture* attachment = dest.texture.get();
            plume::RenderFramebufferDesc fbDesc;
            fbDesc.colorAttachments = &attachment;
            fbDesc.colorAttachmentsCount = 1;
            dest.framebuffer = m_device->createFramebuffer(fbDesc);
            if (!dest.framebuffer) return false;
        }
        Transition(source, plume::RenderTextureLayout::DEPTH_READ);
        m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                plume::RenderTextureBarrier(dest.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
        auto set = m_device->createDescriptorSet(*m_presentSetDesc);
        if (!set) return false;
        set->setTexture(0, source->texture.get(), plume::RenderTextureLayout::DEPTH_READ, source->depthView.get());
        set->setSampler(1, m_presentSampler.get());
        m_commandList->setFramebuffer(dest.framebuffer.get());
        m_currentFramebuffer = nullptr;
        m_commandList->setGraphicsPipelineLayout(m_presentLayout.get());
        m_commandList->setPipeline(m_depthCopyPipeline.get());
        m_commandList->setGraphicsDescriptorSet(set.get(), 0);
        m_commandList->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height)));
        m_commandList->setScissors(rect);
        const float unused[4] = {};
        m_commandList->setGraphicsPushConstants(0, unused, 0, 16);
        m_commandList->drawInstanced(3, 1, 0, 0);
        m_commandList->setFramebuffer(nullptr);
        m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                plume::RenderTextureBarrier(dest.texture.get(), plume::RenderTextureLayout::SHADER_READ));
        m_frameSets.push_back(std::move(set));
        return true;
    }

    void XenosRenderer::Transition(RenderTarget* target, plume::RenderTextureLayout layout) {
        if (target->layout == layout) return;
        m_commandList->barriers(layout == plume::RenderTextureLayout::COPY_SOURCE ? plume::RenderBarrierStage::COPY
                                                                                  : plume::RenderBarrierStage::GRAPHICS,
                                plume::RenderTextureBarrier(target->texture.get(), layout));
        target->layout = layout;
    }

    void XenosRenderer::BindFramebuffer(plume::RenderFramebuffer* framebuffer) {
        if (framebuffer == m_currentFramebuffer) return;
        m_commandList->setFramebuffer(framebuffer);
        m_currentFramebuffer = framebuffer;
    }

    // ------------------------------------------------------------------ draws

    bool XenosRenderer::IssueDraw(const Gpu::DrawCall& draw) {
        if (!draw.vertexShader) {
            ++m_stats.drawsSkipped;
            return false;
        }
        const auto mode = draw.regs.Get<reg::RB_MODECONTROL>().edram_mode;
        if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) {
            ++m_stats.drawsSkipped;
            return true;
        }
        const Gpu::Draw::HostPrimitive primitive = Gpu::Draw::ProcessPrimitive(draw, m_memory);
        if (!primitive.ok) {
            ++m_stats.drawsSkipped;
            LogOnce("primitiva sem suporte: " + std::to_string(uint32_t(draw.primitive)));
            return true;
        }
        ShaderEntry* vs = GetShader(*draw.vertexShader, primitive.rectangleList ? VertexShaderMode::kRectangleList
                                                                                 : VertexShaderMode::kNormal);
        ShaderEntry* ps = (draw.pixelShader && mode == xenos::EdramMode::kColorDepth)
                              ? GetShader(*draw.pixelShader, VertexShaderMode::kNormal)
                              : nullptr;
        if (!vs || (draw.pixelShader && mode == xenos::EdramMode::kColorDepth && !ps)) {
            ++m_stats.drawsSkipped;
            return false;
        }

        Gpu::Draw::RenderState state =
            Gpu::Draw::ComputeRenderState(draw, primitive, ps && ps->translated.writesDepth);
        if (state.scissor.width == 0 || state.scissor.height == 0 || state.surfacePitch == 0) {
            ++m_stats.drawsSkipped;
            return true;
        }

        EnsureRecording();

        // Render targets: cores que o PS escreve (na posicao de cada saida) e profundidade.
        RenderTarget* colors[Gpu::Draw::kMaxColorTargets] = {};
        plume::RenderFormat colorFormats[Gpu::Draw::kMaxColorTargets] = {};
        uint32_t colorCount = 0;
        if (ps) {
            for (uint32_t i = 0; i < Gpu::Draw::kMaxColorTargets; ++i) {
                if (state.color[i].enabled && (ps->translated.colorTargetsWritten & (1u << i))) colorCount = i + 1;
            }
            for (uint32_t i = 0; i < colorCount; ++i) {
                const bool used = state.color[i].enabled && (ps->translated.colorTargetsWritten & (1u << i));
                const plume::RenderFormat format = ColorTargetFormat(state.color[i].format);
                // Lacunas entre saidas usadas recebem um RT descartavel (mascara de escrita zero).
                colors[i] = GetRenderTarget(used ? state.color[i].edramBase : kGapRenderTargetBase + i, state.surfacePitch,
                                            format, false);
                colorFormats[i] = format;
                if (!colors[i]) return false;
            }
        }
        RenderTarget* depth = nullptr;
        if (state.depth.enabled) {
            depth = GetRenderTarget(state.depth.edramBase, state.surfacePitch, plume::RenderFormat::D32_FLOAT_S8_UINT, true);
        }
        if (colorCount == 0 && !depth) {
            ++m_stats.drawsSkipped;
            return true;
        }

        LayoutEntry* layout = GetLayout(*vs, ps);
        if (!layout) return false;
        plume::RenderPipeline* pipeline = GetPipeline(*vs, ps, *layout, state, primitive, colorFormats, colorCount,
                                                      depth ? plume::RenderFormat::D32_FLOAT_S8_UINT : plume::RenderFormat::UNKNOWN);
        if (!pipeline) {
            ++m_stats.drawsSkipped;
            LogOnce("falha ao criar pipeline");
            return false;
        }

        // Texturas antes de qualquer upload do draw (um upload pode reiniciar o buffer).
        struct SetSlot { const plume::RenderTexture* texture; const plume::RenderTextureView* view; const plume::RenderSampler* sampler; };
        std::vector<SetSlot> slots;
        auto collect = [&](const TranslatedShader& shader) {
            for (const Gpu::Shader::TextureBinding& binding : shader.textures) {
                BoundTexture tex = GetTexture(binding.fetchConstant, draw.regs);
                const plume::RenderTexture* mainTexture = tex.texture;
                const plume::RenderTextureView* mainView = tex.view;
                if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
                    const bool is3D = mainTexture && Gpu::Draw::DescribeTexture(draw.regs.TextureFetch(binding.fetchConstant)).dimension ==
                                                         xenos::DataDimension::k3D;
                    if (!is3D) {
                        mainTexture = m_dummy3D.get();
                        mainView = m_dummy3DView.get();
                    }
                } else if (binding.dimension == xenos::FetchOpDimension::kCube) {
                    if (!mainTexture || Gpu::Draw::DescribeTexture(draw.regs.TextureFetch(binding.fetchConstant)).dimension !=
                                            xenos::DataDimension::kCube) {
                        mainTexture = m_dummyCube.get();
                        mainView = m_dummyCubeView.get();
                    }
                } else if (!mainTexture || Gpu::Draw::DescribeTexture(draw.regs.TextureFetch(binding.fetchConstant)).dimension ==
                                               xenos::DataDimension::k3D) {
                    mainTexture = m_dummy2D.get();
                    mainView = m_dummy2DView.get();
                }
                slots.push_back({ mainTexture, mainView, nullptr });
                slots.push_back({ nullptr, nullptr, GetSampler(draw.regs.TextureFetch(binding.fetchConstant), binding) });
                if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
                    slots.push_back({ tex.arrayTexture ? tex.arrayTexture : m_dummy2DArray.get(),
                                      tex.arrayView ? tex.arrayView : m_dummy2DArrayView.get(), nullptr });
                }
            }
        };
        collect(vs->translated);
        if (ps) collect(ps->translated);

        // Bloco de constantes do draw.
        std::vector<uint32_t> block(DrawBlock::kSize, 0);
        for (uint32_t i = 0; i < DrawBlock::kFloatConstantCount * 4; ++i) {
            block[DrawBlock::kFloatConstants + i] = draw.regs[xe::gpu::XE_GPU_REG_SHADER_CONSTANT_000_X + i];
        }
        for (uint32_t i = 0; i < 8; ++i) block[DrawBlock::kBoolConstants + i] = draw.regs[xe::gpu::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i];
        for (uint32_t i = 0; i < 32; ++i) block[DrawBlock::kLoopConstants + i] = draw.regs[xe::gpu::XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
        for (uint32_t i = 0; i < 192; ++i) block[DrawBlock::kFetchConstants + i] = draw.regs[xe::gpu::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i];
        std::copy(state.system.begin(), state.system.end(), block.begin() + DrawBlock::kSystem);

        // Dados dos vertex fetches usados pelo VS e indices: reserva tudo de uma vez.
        struct VertexUpload { uint32_t fc; GuestAddr address; uint32_t bytes; };
        std::vector<VertexUpload> vertexUploads;
        uint64_t needed = DrawBlock::kSize * 4 + 64;
        for (uint32_t fc = 0; fc < xenos::kVertexFetchConstantCount; ++fc) {
            if (!(vs->translated.vertexFetchMask[fc >> 5] & (1u << (fc & 31)))) continue;
            const xenos::xe_gpu_vertex_fetch_t vf = draw.regs.VertexFetch(fc);
            const GuestAddr address = vf.address << 2;
            const uint32_t bytes = vf.size * 4;
            if (!bytes || !m_memory.IsReadable(address, bytes)) continue;
            vertexUploads.push_back({ fc, address, bytes });
            needed += bytes + 16;
        }
        needed += primitive.indices.size() * 4 + 16;
        uint64_t reserveOffset = 0;
        if (!UploadReserve(size_t(needed), 16, reserveOffset)) {
            ++m_stats.drawsSkipped;
            LogOnce("draw maior que o buffer de upload");
            return false;
        }
        m_uploadCursor = reserveOffset;  // devolve: os uploads abaixo usam a area reservada
        if (m_uploadCursor == 0) m_lastBlockOffset = ~0ull;

        for (const VertexUpload& v : vertexUploads) {
            const uint64_t offset = Upload(m_memory.Base() + v.address, v.bytes, 16);
            block[DrawBlock::kVertexRemap + v.fc] = uint32_t(int32_t(offset / 4) - int32_t(v.address / 4));
        }
        uint64_t indexOffset = 0;
        if (!primitive.indices.empty()) indexOffset = Upload(primitive.indices.data(), primitive.indices.size() * 4, 16);
        if (primitive.rectangleList) block[DrawBlock::kSystem + Sys::kIndexRingOffset] = uint32_t(indexOffset / 4);

        uint64_t blockOffset;
        if (m_lastBlockOffset != ~0ull && block == m_lastBlock) {
            blockOffset = m_lastBlockOffset;
        } else {
            blockOffset = Upload(block.data(), block.size() * 4, 16);
            m_lastBlock = block;
            m_lastBlockOffset = blockOffset;
        }

        // Set de texturas do draw: reaproveitada no quadro quando layout e bindings se repetem.
        plume::RenderDescriptorSet* textureSet = nullptr;
        std::vector<const void*> setKey{ layout };
        for (const SetSlot& slot : slots) {
            setKey.push_back(slot.sampler ? static_cast<const void*>(slot.sampler) : static_cast<const void*>(slot.view));
        }
        if (auto cached = m_frameSetCache.find(setKey); !slots.empty() && cached != m_frameSetCache.end()) {
            textureSet = cached->second;
        } else if (!slots.empty()) {
            std::vector<plume::RenderDescriptorRange> all = layout->vsRanges;
            all.insert(all.end(), layout->psRanges.begin(), layout->psRanges.end());
            auto set = m_device->createDescriptorSet(plume::RenderDescriptorSetDesc(all.data(), uint32_t(all.size())));
            if (!set) return false;
            for (uint32_t i = 0; i < slots.size(); ++i) {
                if (slots[i].sampler) {
                    set->setSampler(i, slots[i].sampler);
                } else {
                    set->setTexture(i, slots[i].texture, plume::RenderTextureLayout::SHADER_READ, slots[i].view);
                }
            }
            textureSet = set.get();
            m_frameSetCache.emplace(std::move(setKey), textureSet);
            m_frameSets.push_back(std::move(set));
        }

        // Render targets em layout de escrita (render pass do Plume usa LOAD/STORE).
        for (uint32_t i = 0; i < colorCount; ++i) Transition(colors[i], plume::RenderTextureLayout::COLOR_WRITE);
        if (depth) Transition(depth, plume::RenderTextureLayout::DEPTH_WRITE);
        plume::RenderFramebuffer* framebuffer = GetFramebuffer(colors, colorCount, depth);
        if (!framebuffer) return false;
        BindFramebuffer(framebuffer);

        m_commandList->setGraphicsPipelineLayout(layout->layout.get());
        m_commandList->setPipeline(pipeline);
        m_commandList->setGraphicsDescriptorSet(m_ringSet.get(), 0);
        if (textureSet) m_commandList->setGraphicsDescriptorSet(textureSet, 1);
        const uint32_t cb = uint32_t(blockOffset / 4);
        m_commandList->setGraphicsPushConstants(0, &cb, 0, 4);

        const RenderTarget* sizeSource = colorCount ? colors[0] : depth;
        const Gpu::Draw::Viewport& vp = state.viewport;
        m_commandList->setViewports(plume::RenderViewport(vp.x, vp.y, vp.width, vp.height, vp.zMin, vp.zMax));
        const int32_t right = std::min<int32_t>(state.scissor.x + int32_t(state.scissor.width), int32_t(sizeSource->width));
        const int32_t bottom = std::min<int32_t>(state.scissor.y + int32_t(state.scissor.height), int32_t(sizeSource->height));
        if (right <= state.scissor.x || bottom <= state.scissor.y) {
            ++m_stats.drawsSkipped;
            return true;
        }
        m_commandList->setScissors(plume::RenderRect(state.scissor.x, state.scissor.y, right, bottom));
        m_commandList->setDepthBias(state.polyOffsetConstant, 0.0f, state.polyOffsetScale);

        if (primitive.rectangleList) {
            m_commandList->drawInstanced(primitive.vertexCount, 1, 0, 0);
        } else if (primitive.indexed) {
            const plume::RenderIndexBufferView view(plume::RenderBufferReference(m_upload.get(), indexOffset),
                                                    uint32_t(primitive.indices.size() * 4), plume::RenderFormat::R32_UINT);
            m_commandList->setIndexBuffer(&view);
            m_commandList->drawIndexedInstanced(uint32_t(primitive.indices.size()), 1, 0, 0, 0);
        } else {
            m_commandList->drawInstanced(primitive.vertexCount, 1, 0, 0);
        }
        for (uint32_t i = 0; i < colorCount; ++i) colors[i]->used = true;
        if (depth) depth->used = true;
        ++m_stats.draws;
        return true;
    }

    // ------------------------------------------------------------------ resolves

    bool XenosRenderer::IssueCopy(const Gpu::RegisterFileView& regs) {
        const Gpu::Draw::ResolveInfo info = Gpu::Draw::ComputeResolveInfo(regs, m_memory);
        if (!info.valid) {
            LogOnce("resolve com vertices invalidos (ignorado)");
            return false;
        }
        EnsureRecording();
        ++m_stats.resolves;

        RenderTarget* source = nullptr;
        if (info.sourceIsDepth) {
            source = GetRenderTarget(info.sourceEdramBase, info.surfacePitch, plume::RenderFormat::D32_FLOAT_S8_UINT, true);
        } else {
            source = GetRenderTarget(info.sourceEdramBase, info.surfacePitch, ColorTargetFormat(info.sourceColorFormat), false);
        }
        if (!source) return false;

        if (info.copy) {
            // Copia o retangulo para uma textura do host associada ao endereco de destino. A
            // profundidade vira R32F (k_24_8 lido como float pelos shaders).
            const plume::RenderFormat format = info.sourceIsDepth ? plume::RenderFormat::R32_FLOAT : source->format;
            const uint32_t width = std::max(info.destPitch, info.x + info.width);
            const uint32_t height = std::max(std::max(info.destHeight, 1u), info.y + info.height);
            auto& slot = m_resolved[info.destBase];
            if (!slot || slot->format != format || slot->width != width || slot->height != height) {
                // A anterior pode estar em comandos ja gravados: so e destruida no fim do quadro.
                if (slot) m_retiredResolved.push_back(std::move(slot));
                slot = std::make_unique<ResolvedTexture>();
                slot->format = format;
                slot->width = width;
                slot->height = height;
                slot->texture = info.sourceIsDepth
                                    ? m_device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, format))
                                    : m_device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, format));
                if (!slot->texture) {
                    m_resolved.erase(info.destBase);
                    return false;
                }
                slot->view = slot->texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
            }
            const uint32_t copyRight = std::min(info.x + info.width, source->width);
            const uint32_t copyBottom = std::min(info.y + info.height, source->height);
            if (info.sourceIsDepth && copyRight > info.x && copyBottom > info.y) {
                CopyDepth(source, *slot, plume::RenderRect(int32_t(info.x), int32_t(info.y), int32_t(copyRight), int32_t(copyBottom)));
            } else if (copyRight > info.x && copyBottom > info.y) {
                Transition(source, plume::RenderTextureLayout::COPY_SOURCE);
                m_commandList->barriers(plume::RenderBarrierStage::COPY,
                                        plume::RenderTextureBarrier(slot->texture.get(), plume::RenderTextureLayout::COPY_DEST));
                const plume::RenderBox box(int32_t(info.x), int32_t(info.y), int32_t(copyRight), int32_t(copyBottom));
                m_commandList->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(slot->texture.get()),
                                                 plume::RenderTextureCopyLocation::Subresource(source->texture.get()), info.x,
                                                 info.y, 0, &box);
                m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                        plume::RenderTextureBarrier(slot->texture.get(), plume::RenderTextureLayout::SHADER_READ));
            }
            // Uma textura do cache no mesmo endereco agora esta obsoleta.
            for (auto it = m_textures.begin(); it != m_textures.end();) {
                if (it->second->desc.baseAddress == info.destBase) {
                    m_retiredTextures.push_back(std::move(it->second));
                    it = m_textures.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Clears depois da copia.
        if (info.width && info.height && (info.clearColor || info.clearDepth)) {
            const plume::RenderRect rect(int32_t(info.x), int32_t(info.y), int32_t(info.x + info.width),
                                         int32_t(info.y + info.height));
            if (info.clearColor && !info.sourceIsDepth) {
                Transition(source, plume::RenderTextureLayout::COLOR_WRITE);
                RenderTarget* targets[1] = { source };
                BindFramebuffer(GetFramebuffer(targets, 1, nullptr));
                const auto c = Gpu::Draw::UnpackColorClear(info.sourceColorFormat, info.colorClear, info.colorClearLo);
                m_commandList->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]), &rect, 1);
                ++m_stats.clears;
            }
            if (info.clearDepth) {
                const auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
                RenderTarget* depth =
                    GetRenderTarget(depthInfo.depth_base, info.surfacePitch, plume::RenderFormat::D32_FLOAT_S8_UINT, true);
                if (depth) {
                    Transition(depth, plume::RenderTextureLayout::DEPTH_WRITE);
                    BindFramebuffer(GetFramebuffer(nullptr, 0, depth));
                    float d;
                    uint32_t stencil;
                    Gpu::Draw::UnpackDepthClear(depthInfo.depth_format, info.depthClear, d, stencil);
                    m_commandList->clearDepthStencil(true, true, d, stencil, &rect, 1);
                    ++m_stats.clears;
                }
            }
        }
        return true;
    }

    // ------------------------------------------------------------------ apresentacao

    plume::RenderCommandList* XenosRenderer::RecordPresent(plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer,
                                                           uint32_t targetWidth, uint32_t targetHeight, uint32_t frontbufferAddr,
                                                           uint32_t frontbufferWidth, uint32_t frontbufferHeight,
                                                           const float clearColor[4]) {
        EnsureRecording();
        m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
        m_commandList->setFramebuffer(framebuffer);
        m_currentFramebuffer = nullptr;
        m_commandList->setViewports(plume::RenderViewport(0.0f, 0.0f, float(targetWidth), float(targetHeight)));
        m_commandList->setScissors(plume::RenderRect(0, 0, int32_t(targetWidth), int32_t(targetHeight)));

        auto resolved = m_resolved.find(frontbufferAddr);
        if (resolved != m_resolved.end()) {
            auto set = m_device->createDescriptorSet(*m_presentSetDesc);
            set->setTexture(0, resolved->second->texture.get(), plume::RenderTextureLayout::SHADER_READ, resolved->second->view.get());
            set->setSampler(1, m_presentSampler.get());
            m_commandList->setGraphicsPipelineLayout(m_presentLayout.get());
            m_commandList->setPipeline(m_presentPipeline.get());
            m_commandList->setScissors(plume::RenderRect(0, 0, int32_t(targetWidth), int32_t(targetHeight)));
            m_commandList->setGraphicsDescriptorSet(set.get(), 0);
            // O frontbuffer pode ser menor que a textura resolvida (pitch alinhado).
            const ResolvedTexture& fb = *resolved->second;
            const float scale[4] = {
                frontbufferWidth ? std::min(1.0f, float(frontbufferWidth) / float(fb.width)) : 1.0f,
                frontbufferHeight ? std::min(1.0f, float(frontbufferHeight) / float(fb.height)) : 1.0f,
                0.0f, 0.0f,
            };
            m_commandList->setGraphicsPushConstants(0, scale, 0, 16);
            m_commandList->drawInstanced(3, 1, 0, 0);
            m_frameSets.push_back(std::move(set));
            ++m_stats.presentsFromResolve;
        } else {
            m_commandList->clearColor(0, plume::RenderColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]));
        }
        m_commandList->barriers(plume::RenderBarrierStage::NONE,
                                plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
        m_commandList->setFramebuffer(nullptr);
        m_commandList->end();
        m_recording = false;
        return m_commandList.get();
    }

    bool XenosRenderer::ReadbackResolved(uint32_t address, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
        auto it = m_resolved.find(address);
        if (it == m_resolved.end()) return false;
        // 4 bytes por pixel: RGBA8 (cor) ou R32F (profundidade).
        if (it->second->format != plume::RenderFormat::R8G8B8A8_UNORM && it->second->format != plume::RenderFormat::R32_FLOAT) return false;
        ResolvedTexture& r = *it->second;
        width = r.width;
        height = r.height;
        const uint32_t rowPitch = (width * 4 + 255) & ~255u;
        auto readback = m_device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(rowPitch) * height));
        if (!readback) return false;
        EnsureRecording();
        m_commandList->barriers(plume::RenderBarrierStage::COPY,
                                plume::RenderTextureBarrier(r.texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
        m_commandList->copyTextureRegion(
            plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), r.format, width, height, 1, rowPitch / 4, 0),
            plume::RenderTextureCopyLocation::Subresource(r.texture.get()));
        m_commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                plume::RenderTextureBarrier(r.texture.get(), plume::RenderTextureLayout::SHADER_READ));
        SubmitAndWait();
        const uint8_t* mapped = static_cast<const uint8_t*>(readback->map());
        if (!mapped) return false;
        rgba.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) std::memcpy(rgba.data() + size_t(y) * width * 4, mapped + size_t(y) * rowPitch, width * 4);
        readback->unmap();
        return true;
    }
}
