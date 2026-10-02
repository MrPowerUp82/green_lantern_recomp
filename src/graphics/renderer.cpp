#include "renderer.h"
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Graphics {
    RenderConfig NativeRenderer::s_config{};
    bool NativeRenderer::s_initialized = false;
    bool NativeRenderer::s_running = false;

#if defined(_WIN32)
    static HWND s_hwnd = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
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

    bool NativeRenderer::Initialize(const RenderConfig& config) {
        s_config = config;
        std::cout << "====================================================\n";
        std::cout << "[Renderer] Inicializando Renderizador Nativo ("
                  << (s_config.backend == Backend::Direct3D12 ? "Direct3D 12" : "Vulkan") 
                  << ")...\n";
        std::cout << "[Renderer] Resolução: " << s_config.width << "x" << s_config.height << "\n";
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
#endif

        s_initialized = true;
        s_running = true;

        std::cout << "[Renderer] Janela e contexto gráfico nativo criados com sucesso." << std::endl;
        return true;
    }

    void NativeRenderer::Shutdown() {
        if (!s_initialized) return;

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

    void NativeRenderer::BeginFrame() {
        PollEvents();
        // Limpeza de render targets e preparação do command list moderno
    }

    void NativeRenderer::EndFrame() {
        // Apresentação da swapchain (Flip Model em D3D12 / vkQueuePresentKHR em Vulkan)
    }
}
