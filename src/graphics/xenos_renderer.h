#pragma once

#include "gpu/draw/draw_state.h"
#include "gpu/draw/primitive_processor.h"
#include "gpu/draw/resolve_info.h"
#include "gpu/draw/texture_loader.h"
#include "gpu/draw_call.h"
#include "gpu/guest_memory.h"
#include "gpu/shader/glsl_translator.h"

#include "plume_render_interface.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Graphics {
    struct XenosRendererStats {
        uint64_t draws = 0;
        uint64_t drawsSkipped = 0;  // sem shader, estado sem suporte ou area vazia
        uint64_t resolves = 0;
        uint64_t clears = 0;
        uint64_t texturesUploaded = 0;
        uint64_t shadersCompiled = 0;
        uint64_t shaderFailures = 0;
        uint64_t pipelinesCreated = 0;
        uint64_t presentsFromResolve = 0;
    };

    // Executa os draws do Xenos no device Plume (Vulkan): traduz shaders, mantem render targets do
    // EDRAM, texturas, resolves e apresenta o frontbuffer. Roda inteiro na thread do CP.
    // Os comandos de um quadro vao numa command list submetida no XE_SWAP (ou antes, se o buffer
    // de upload encher).
    class XenosRenderer {
    public:
        XenosRenderer(plume::RenderDevice* device, plume::RenderCommandQueue* queue, uint8_t* guestBase);
        ~XenosRenderer();
        XenosRenderer(const XenosRenderer&) = delete;
        XenosRenderer& operator=(const XenosRenderer&) = delete;

        bool Initialize();

        bool IssueDraw(const Gpu::DrawCall& draw);
        bool IssueCopy(const Gpu::RegisterFileView& regs);

        // Grava a apresentacao do frontbuffer (resolvido em `frontbufferAddr`) na imagem `target`
        // da swapchain, ja em COLOR_WRITE, e fecha o quadro. Retorna a command list do quadro, que o
        // chamador submete com os semaforos da swapchain; depois chame EndFrame().
        plume::RenderCommandList* RecordPresent(plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer,
                                                uint32_t targetWidth, uint32_t targetHeight, uint32_t frontbufferAddr,
                                                uint32_t frontbufferWidth, uint32_t frontbufferHeight,
                                                const float clearColor[4]);
        // Chamado depois que a GPU terminou o quadro (fence): recicla o buffer de upload.
        void EndFrame();
        // Submete e espera o que estiver gravado (sem apresentar). Usado no shutdown e em capturas.
        void Flush();

        // Le de volta a textura resolvida em `address` (4 bytes/pixel: RGBA8 ou R32F). Para o selftest.
        bool ReadbackResolved(uint32_t address, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);

        const XenosRendererStats& Stats() const { return m_stats; }

    private:
        struct ShaderEntry {
            Gpu::Shader::TranslatedShader translated;
            std::unique_ptr<plume::RenderShader> shader;
            std::unique_ptr<plume::RenderDescriptorSetDesc> setDesc;  // descritor da set de texturas
            std::vector<plume::RenderDescriptorRange> ranges;
            std::string layoutKey;
            bool ok = false;
        };

        struct LayoutEntry {
            std::unique_ptr<plume::RenderPipelineLayout> layout;
            std::vector<plume::RenderDescriptorRange> vsRanges, psRanges;
        };

        struct RenderTarget {
            std::unique_ptr<plume::RenderTexture> texture;
            plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
            uint32_t width = 0, height = 0;
            bool isDepth = false;
            bool used = false;  // ja recebeu draw/clear desde a criacao
            plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
            std::unique_ptr<plume::RenderTextureView> depthView;  // para copiar a profundidade
        };

        struct CachedTexture {
            std::unique_ptr<plume::RenderTexture> texture;
            std::unique_ptr<plume::RenderTextureView> view;        // 2D (camada 0), 3D ou cubo
            std::unique_ptr<plume::RenderTextureView> arrayView;   // 2D array (empilhada)
            Gpu::Draw::TextureDescription desc;
            uint64_t contentHash = 0;
            uint64_t validatedFrame = ~0ull;
        };

        struct ResolvedTexture {
            std::unique_ptr<plume::RenderTexture> texture;
            std::unique_ptr<plume::RenderTextureView> view;
            std::unique_ptr<plume::RenderFramebuffer> framebuffer;  // so para resolves de profundidade
            plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
            uint32_t width = 0, height = 0;
        };

        struct BoundTexture {
            const plume::RenderTexture* texture = nullptr;
            const plume::RenderTextureView* view = nullptr;
            const plume::RenderTexture* arrayTexture = nullptr;
            const plume::RenderTextureView* arrayView = nullptr;
        };

        // Recursos do quadro.
        bool EnsureRecording();
        void SubmitAndWait();
        uint64_t Upload(const void* data, size_t size, size_t alignment);
        void* UploadReserve(size_t size, size_t alignment, uint64_t& offset);

        ShaderEntry* GetShader(const Gpu::ShaderBlob& blob, Gpu::Shader::VertexShaderMode mode);
        LayoutEntry* GetLayout(const ShaderEntry& vs, const ShaderEntry* ps);
        plume::RenderPipeline* GetPipeline(const ShaderEntry& vs, const ShaderEntry* ps, const LayoutEntry& layout,
                                           const Gpu::Draw::RenderState& state, const Gpu::Draw::HostPrimitive& primitive,
                                           plume::RenderFormat colorFormats[4], uint32_t colorCount,
                                           plume::RenderFormat depthFormat);
        RenderTarget* GetRenderTarget(uint32_t edramBase, uint32_t pitch, plume::RenderFormat format, bool isDepth);
        plume::RenderFramebuffer* GetFramebuffer(RenderTarget* const* colors, uint32_t colorCount, RenderTarget* depth);
        BoundTexture GetTexture(uint32_t fetchConstant, const Gpu::RegisterFileView& regs);
        plume::RenderSampler* GetSampler(const xe::gpu::xenos::xe_gpu_texture_fetch_t& fetch,
                                         const Gpu::Shader::TextureBinding& binding);
        bool InitializePresent();
        // Copia de profundidade (D32S8 -> R32F) por um passe de tela cheia, para resolves de depth.
        bool CopyDepth(RenderTarget* source, ResolvedTexture& dest, const plume::RenderRect& rect);
        // Barreira so quando o layout muda (cada barreira do Plume encerra o render pass).
        void Transition(RenderTarget* target, plume::RenderTextureLayout layout);
        void BindFramebuffer(plume::RenderFramebuffer* framebuffer);

        plume::RenderDevice* m_device;
        plume::RenderCommandQueue* m_queue;
        Gpu::GuestMemory m_memory;

        std::unique_ptr<plume::RenderCommandList> m_commandList;
        std::unique_ptr<plume::RenderCommandFence> m_fence;
        bool m_recording = false;
        uint64_t m_frame = 0;

        // Buffer de upload (constantes, vertices, indices, texturas): host-visible, recomecado a cada quadro.
        std::unique_ptr<plume::RenderBuffer> m_upload;
        uint8_t* m_uploadMapped = nullptr;
        uint64_t m_uploadSize = 0;
        uint64_t m_uploadCursor = 0;
        std::unique_ptr<plume::RenderDescriptorSet> m_ringSet;
        plume::RenderDescriptorRange m_ringRange;
        std::unique_ptr<plume::RenderDescriptorSetDesc> m_ringSetDesc;

        // Ultimo bloco de constantes enviado (reaproveitado quando o draw seguinte nao muda nada).
        std::vector<uint32_t> m_lastBlock;
        uint64_t m_lastBlockOffset = ~0ull;

        std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> m_shaders;
        std::map<std::string, std::unique_ptr<LayoutEntry>> m_layouts;
        std::unordered_map<std::string, std::unique_ptr<plume::RenderPipeline>> m_pipelines;
        std::map<std::tuple<uint32_t, uint32_t, int, bool>, std::unique_ptr<RenderTarget>> m_renderTargets;
        std::map<std::vector<const void*>, std::unique_ptr<plume::RenderFramebuffer>> m_framebuffers;
        std::unordered_map<uint64_t, std::unique_ptr<CachedTexture>> m_textures;
        std::unordered_map<uint64_t, std::unique_ptr<plume::RenderSampler>> m_samplers;
        std::map<uint32_t, std::unique_ptr<ResolvedTexture>> m_resolved;
        std::vector<std::unique_ptr<plume::RenderDescriptorSet>> m_frameSets;  // liberadas no EndFrame
        std::map<std::vector<const void*>, plume::RenderDescriptorSet*> m_frameSetCache;
        // Texturas substituidas no meio do quadro, destruidas depois do fence.
        std::vector<std::unique_ptr<CachedTexture>> m_retiredTextures;
        std::vector<std::unique_ptr<ResolvedTexture>> m_retiredResolved;

        // Texturas vazias para bindings sem textura valida.
        std::unique_ptr<plume::RenderTexture> m_dummy2D, m_dummy2DArray, m_dummy3D, m_dummyCube;
        std::unique_ptr<plume::RenderTextureView> m_dummy2DView, m_dummy2DArrayView, m_dummy3DView, m_dummyCubeView;
        std::unique_ptr<plume::RenderSampler> m_defaultSampler;

        // Apresentacao: triangulo de tela cheia amostrando o frontbuffer.
        std::unique_ptr<plume::RenderShader> m_presentVs, m_presentPs;
        std::unique_ptr<plume::RenderPipelineLayout> m_presentLayout;
        std::unique_ptr<plume::RenderDescriptorSetDesc> m_presentSetDesc;
        std::vector<plume::RenderDescriptorRange> m_presentRanges;
        std::unique_ptr<plume::RenderPipeline> m_presentPipeline;
        std::unique_ptr<plume::RenderSampler> m_presentSampler;
        std::unique_ptr<plume::RenderShader> m_depthCopyPs;
        std::unique_ptr<plume::RenderPipeline> m_depthCopyPipeline;

        plume::RenderFramebuffer* m_currentFramebuffer = nullptr;
        XenosRendererStats m_stats;
    };
}
