#include <iostream>
#include "types.h"
#include "platform/filesystem.h"
#include "kernel/memory.h"
#include "kernel/kernel_stubs.h"
#include "kernel/guest_thread.h"
#include "kernel/guest_call.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include "graphics/renderer.h"
#include "gpu/gpu_system.h"

int main(int argc, char** argv) {
    // --no-gpu-interrupts: nao dispara o callback guest de vblank/INTERRUPT da GPU.
    bool gpuInterrupts = true;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-gpu-interrupts") == 0) gpuInterrupts = false;
    }

    std::cout << "====================================================\n";
    std::cout << " Green Lantern: Rise of the Manhunters - PC Recomp \n";
    std::cout << " Native Recompilation Engine (Vulkan / Direct3D 12)  \n";
    std::cout << "====================================================\n\n";

    // 1. Inicializar Virtual File System
    Platform::VFS::Initialize("./game");

    // 2. Inicializar o espaço de memória virtual de 4 GB do console
    if (!Kernel::MemoryManager::Initialize()) {
        std::cerr << "[Main] Falha ao inicializar o gerenciador de memória do console." << std::endl;
        return 1;
    }

    // 3. Carregar seções do binário original descompactado no espaço 0x82000000
    auto pePath = Platform::VFS::ResolvePath("game:/default_unpacked.pe");
    if (std::filesystem::exists(pePath)) {
        Kernel::MemoryManager::LoadExecutableImage(pePath);
    } else {
        std::cerr << "[Main] Atenção: Imagem " << pePath << " não encontrada. Rode tools/unpack_xex.py primeiro.\n";
    }

    // 4. Mapear tabela de despacho de funções (PPCFuncMappings -> O(1) direct call table)
    Kernel::MemoryManager::InitializeFunctionDispatchTable();

    // 5. Inicializar chamadas HLE de Sistema Operacional (XboxKrnl & XAM)
    Kernel::InitializeExports();

    // 6. Teste de resolução de arquivo de nível
    auto mainMenuLvl = Platform::VFS::ResolvePath("game:/gl/common/gl_mainmenu.lvl_760");
    std::cout << "\n[Main] Verificando existência de menu principal: " 
              << mainMenuLvl.string() << " -> " 
              << (std::filesystem::exists(mainMenuLvl) ? "ENCONTRADO (OK)" : "NÃO ENCONTRADO") 
              << std::endl;

    // 7. Inicializar o Renderizador Nativo (Vulkan por padrão com fallback para Direct3D 12)
    Graphics::RenderConfig renderConfig;
    renderConfig.width = 1280;
    renderConfig.height = 720;
    renderConfig.backend = Graphics::Backend::Vulkan;

    if (!Graphics::NativeRenderer::Initialize(renderConfig)) {
        std::cerr << "[Main] Falha ao inicializar o renderizador nativo." << std::endl;
        return 1;
    }

    // 8. GPU emulada: o command processor consome o ring PM4 do jogo e chama o renderer a cada swap.
    //    Os imports Vd* (src/kernel/vd_exports.cpp) acessam este estado via GpuSystem::Get().
    static Graphics::RendererBackend rendererBackend;
    auto allocateGuest = [](uint32_t size) { return Kernel::MemoryManager::AllocateVirtual(size); };

    Gpu::GpuSystemConfig gpuConfig;
    gpuConfig.base = Kernel::MemoryManager::GetBase();
    gpuConfig.backend = &rendererBackend;
    gpuConfig.allocate = allocateGuest;
    gpuConfig.caller = Kernel::CreateGuestCaller(gpuConfig.base, allocateGuest);
    gpuConfig.enableInterrupts = gpuInterrupts;
    if (!Gpu::GpuSystem::Get().Initialize(gpuConfig)) {
        std::cerr << "[Main] Falha ao inicializar a GPU emulada." << std::endl;
        Graphics::NativeRenderer::Shutdown();
        return 1;
    }

    std::cout << "\n[Main] Sistema completamente inicializado e pronto para execução.\n";
    std::cout << "[Main] Feche a janela gráfica ou pressione ESC para encerrar o teste de runtime.\n";

    // 9. Iniciar a thread principal do jogo (entry point 0x822FC750 = _xstart)
    if (!Kernel::GuestThread::StartMain()) {
        std::cerr << "[Main] Falha ao iniciar a thread principal do guest." << std::endl;
        Gpu::GpuSystem::Get().Shutdown();
        Graphics::NativeRenderer::Shutdown();
        return 1;
    }

    // Loop da thread principal: so a janela. A apresentacao acontece na thread do CP (pacote XE_SWAP).
    while (Graphics::NativeRenderer::IsRunning() && Kernel::GuestThread::IsRunning()) {
        Graphics::NativeRenderer::PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (Kernel::GuestThread::IsRunning()) {
        // Janela fechada com o guest ainda executando: nao liberar a memoria sob a thread
        std::cout << "[Main] Encerrando com a thread do guest ativa." << std::endl;
        Gpu::GpuSystem::Get().Shutdown();
        Graphics::NativeRenderer::Shutdown();
        std::_Exit(0);
    }

    Gpu::GpuSystem::Get().Shutdown();
    Graphics::NativeRenderer::Shutdown();
    Kernel::MemoryManager::Shutdown();

    std::cout << "[Main] Execução finalizada com sucesso." << std::endl;
    return 0;
}
