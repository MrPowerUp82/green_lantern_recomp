#include "renderer.h"

#include <iostream>
#include <vector>

#include "plume_render_interface.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace plume {
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#if defined(_WIN32)
    extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}

namespace Graphics {
    RenderConfig NativeRenderer::s_config{};
    bool NativeRenderer::s_initialized = false;
    bool NativeRenderer::s_running = false;

    namespace {
        constexpr uint32_t kBufferCount = 2;
        constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::B8G8R8A8_UNORM;

        // Todo o estado GPU. A ordem de declaracao define a ordem inversa de destruicao
        // (framebuffers/semaforos antes da swapchain, swapchain antes da fila, fila antes do device).
        struct GpuContext {
            std::unique_ptr<plume::RenderInterface> interface;
            std::unique_ptr<plume::RenderDevice> device;
            std::unique_ptr<plume::RenderCommandQueue> queue;
            std::unique_ptr<plume::RenderSwapChain> swapChain;
            std::unique_ptr<plume::RenderCommandList> commandList;
            std::unique_ptr<plume::RenderCommandFence> fence;
            std::unique_ptr<plume::RenderCommandSemaphore> acquireSemaphore;
            std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> releaseSemaphores;
            std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
        };

        std::unique_ptr<GpuContext> s_gpu;
        bool s_frameActive = false;
        uint32_t s_imageIndex = 0;
        bool s_resizePending = false;
        bool s_minimized = false;

#if defined(_WIN32)
        HWND s_hwnd = nullptr;

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            switch (msg) {
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                s_minimized = (wParam == SIZE_MINIMIZED);
                s_resizePending = true;
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    PostQuitMessage(0);
                }
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }
#endif

        const char* BackendToString(Backend backend) {
            return backend == Backend::Vulkan ? "Vulkan" : "Direct3D 12";
        }

        std::unique_ptr<plume::RenderInterface> CreateInterface(Backend backend) {
            switch (backend) {
            case Backend::Vulkan:
                return plume::CreateVulkanInterface();
            case Backend::Direct3D12:
#if defined(_WIN32)
                return plume::CreateD3D12Interface();
#else
                return nullptr;
#endif
            }
            return nullptr;
        }

        void CreateFramebuffers(GpuContext& gpu) {
            gpu.framebuffers.clear();
            for (uint32_t i = 0; i < gpu.swapChain->getTextureCount(); i++) {
                const plume::RenderTexture* color = gpu.swapChain->getTexture(i);

                plume::RenderFramebufferDesc desc;
                desc.colorAttachments = &color;
                desc.colorAttachmentsCount = 1;
                desc.depthAttachment = nullptr;
                gpu.framebuffers.push_back(gpu.device->createFramebuffer(desc));
            }

            while (gpu.releaseSemaphores.size() < gpu.swapChain->getTextureCount()) {
                gpu.releaseSemaphores.emplace_back(gpu.device->createCommandSemaphore());
            }
        }

        // Cria interface -> device -> fila -> swapchain para o backend pedido.
        bool CreateGpuContext(Backend backend, plume::RenderWindow window, bool vsync) {
            auto gpu = std::make_unique<GpuContext>();

            gpu->interface = CreateInterface(backend);
            if (!gpu->interface) {
                std::cerr << "[Renderer] " << BackendToString(backend)
                          << " indisponivel (driver/loader nao encontrado)." << std::endl;
                return false;
            }

            gpu->device = gpu->interface->createDevice();
            if (!gpu->device) {
                std::cerr << "[Renderer] Falha ao criar o dispositivo " << BackendToString(backend) << "." << std::endl;
                return false;
            }

            gpu->queue = gpu->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
            if (!gpu->queue) {
                std::cerr << "[Renderer] Falha ao criar a fila de comandos." << std::endl;
                return false;
            }

            gpu->swapChain = gpu->queue->createSwapChain(plume::RenderSwapChainDesc(window, kSwapchainFormat, kBufferCount));
            if (!gpu->swapChain) {
                std::cerr << "[Renderer] Falha ao criar a swapchain." << std::endl;
                return false;
            }
            gpu->swapChain->setVsyncEnabled(vsync);
            if (!gpu->swapChain->resize()) {
                std::cerr << "[Renderer] Falha ao dimensionar a swapchain." << std::endl;
                return false;
            }

            gpu->commandList = gpu->queue->createCommandList();
            gpu->fence = gpu->device->createCommandFence();
            gpu->acquireSemaphore = gpu->device->createCommandSemaphore();
            if (!gpu->commandList || !gpu->fence || !gpu->acquireSemaphore) {
                std::cerr << "[Renderer] Falha ao criar objetos de sincronizacao." << std::endl;
                return false;
            }

            CreateFramebuffers(*gpu);

            const plume::RenderDeviceDescription& desc = gpu->device->getDescription();
            std::cout << "[Renderer] GPU: " << desc.name << std::endl;

            s_gpu = std::move(gpu);
            return true;
        }

        bool ResizeSwapChain() {
            GpuContext& gpu = *s_gpu;
            // Libera referencias as imagens antigas antes de recriar a swapchain.
            gpu.framebuffers.clear();
            const bool ok = gpu.swapChain->resize();
            if (ok) {
                CreateFramebuffers(gpu);
            }
            return ok;
        }
    }

    bool NativeRenderer::Initialize(const RenderConfig& config) {
        s_config = config;
        std::cout << "====================================================\n";
        std::cout << "[Renderer] Inicializando Renderizador Nativo (" << BackendToString(s_config.backend) << ")...\n";
        std::cout << "[Renderer] Resolucao: " << s_config.width << "x" << s_config.height << "\n";
        std::cout << "====================================================\n";

#if defined(_WIN32)
        HINSTANCE hInstance = GetModuleHandle(nullptr);
        const char* className = "GreenLanternRecompWindowClass";

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className;

        RegisterClassExA(&wc);

        RECT rect = { 0, 0, static_cast<LONG>(s_config.width), static_cast<LONG>(s_config.height) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        s_hwnd = CreateWindowExA(
            0,
            className,
            "Green Lantern: Rise of the Manhunters - Native PC Port",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr, nullptr, hInstance, nullptr
        );

        if (!s_hwnd) {
            std::cerr << "[Renderer] Falha ao criar a janela Win32 nativa." << std::endl;
            return false;
        }

        ShowWindow(s_hwnd, SW_SHOW);
        UpdateWindow(s_hwnd);

        const plume::RenderWindow window = s_hwnd;
#else
        std::cerr << "[Renderer] Plataforma sem suporte a janela nativa." << std::endl;
        return false;
#endif

        Backend backends[2] = {
            s_config.backend,
            s_config.backend == Backend::Vulkan ? Backend::Direct3D12 : Backend::Vulkan
        };
        const int attempts = s_config.allowBackendFallback ? 2 : 1;

        bool created = false;
        for (int i = 0; i < attempts && !created; i++) {
            if (i > 0) {
                std::cout << "[Renderer] Tentando fallback para " << BackendToString(backends[i]) << "..." << std::endl;
            }
            created = CreateGpuContext(backends[i], window, s_config.vsync);
            if (created) {
                s_config.backend = backends[i];
            }
        }

        if (!created) {
#if defined(_WIN32)
            DestroyWindow(s_hwnd);
            s_hwnd = nullptr;
#endif
            return false;
        }

        s_resizePending = false;
        s_initialized = true;
        s_running = true;

        std::cout << "[Renderer] Janela e contexto grafico nativo (" << GetBackendName()
                  << ") criados com sucesso." << std::endl;
        return true;
    }

    void NativeRenderer::Shutdown() {
        if (!s_initialized) return;

        if (s_gpu) {
            if (s_frameActive) {
                // Frame aberto sem EndFrame: fecha a gravacao para liberar o command list.
                s_gpu->commandList->end();
                s_frameActive = false;
            }
            // Cada EndFrame espera o fence, logo a GPU ja esta ociosa aqui.
            s_gpu.reset();
        }

#if defined(_WIN32)
        if (s_hwnd) {
            DestroyWindow(s_hwnd);
            s_hwnd = nullptr;
        }
#endif
        s_initialized = false;
        s_running = false;
        std::cout << "[Renderer] Renderizador nativo finalizado." << std::endl;
    }

    void NativeRenderer::PollEvents() {
#if defined(_WIN32)
        MSG msg = {};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
#endif
    }

    bool NativeRenderer::IsRunning() {
        return s_running;
    }

    const char* NativeRenderer::GetBackendName() {
        return BackendToString(s_config.backend);
    }

    void NativeRenderer::BeginFrame() {
        PollEvents();

        if (!s_initialized || !s_running || !s_gpu || s_minimized) {
            return;
        }

        GpuContext& gpu = *s_gpu;

        if (s_resizePending || gpu.swapChain->needsResize()) {
            s_resizePending = false;
            if (!ResizeSwapChain() || gpu.swapChain->isEmpty()) {
                return;
            }
        }

        if (!gpu.swapChain->acquireTexture(gpu.acquireSemaphore.get(), &s_imageIndex)) {
            // Swapchain desatualizada (VK_ERROR_OUT_OF_DATE_KHR): recria no proximo frame.
            s_resizePending = true;
            return;
        }

        gpu.commandList->begin();

        plume::RenderTexture* target = gpu.swapChain->getTexture(s_imageIndex);
        gpu.commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                  plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
        gpu.commandList->setFramebuffer(gpu.framebuffers[s_imageIndex].get());

        const uint32_t width = gpu.swapChain->getWidth();
        const uint32_t height = gpu.swapChain->getHeight();
        gpu.commandList->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
        gpu.commandList->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));

        const float* c = s_config.clearColor;
        gpu.commandList->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]));

        s_frameActive = true;
    }

    void NativeRenderer::EndFrame() {
        if (!s_frameActive) {
            return;
        }
        s_frameActive = false;

        GpuContext& gpu = *s_gpu;

        plume::RenderTexture* target = gpu.swapChain->getTexture(s_imageIndex);
        gpu.commandList->barriers(plume::RenderBarrierStage::NONE,
                                  plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
        gpu.commandList->end();

        const plume::RenderCommandList* commandList = gpu.commandList.get();
        plume::RenderCommandSemaphore* waitSemaphore = gpu.acquireSemaphore.get();
        plume::RenderCommandSemaphore* signalSemaphore = gpu.releaseSemaphores[s_imageIndex].get();

        gpu.queue->executeCommandLists(&commandList, 1, &waitSemaphore, 1, &signalSemaphore, 1, gpu.fence.get());

        // Apresentacao da swapchain (vkQueuePresentKHR no backend Vulkan).
        if (!gpu.swapChain->present(s_imageIndex, &signalSemaphore, 1)) {
            s_resizePending = true;
        }
        gpu.queue->waitForCommandFence(gpu.fence.get());
    }
}
