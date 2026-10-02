#include <iostream>
#include "types.h"
#include "platform/filesystem.h"
#include "kernel/memory.h"
#include "kernel/kernel_stubs.h"
#include "graphics/renderer.h"

int main(int argc, char** argv) {
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

    // 7. Inicializar o Renderizador Nativo (Direct3D 12 por padrão com fallback para Vulkan)
    Graphics::RenderConfig renderConfig;
    renderConfig.width = 1280;
    renderConfig.height = 720;
    renderConfig.backend = Graphics::Backend::Direct3D12;

    if (!Graphics::NativeRenderer::Initialize(renderConfig)) {
        std::cerr << "[Main] Falha ao inicializar o renderizador nativo." << std::endl;
        return 1;
    }

    std::cout << "\n[Main] Sistema completamente inicializado e pronto para execução.\n";
    std::cout << "[Main] Feche a janela gráfica ou pressione ESC para encerrar o teste de runtime.\n";

    // Loop de apresentação básica de teste
    int frameCount = 0;
    while (Graphics::NativeRenderer::IsRunning() && frameCount < 60) {
        Graphics::NativeRenderer::BeginFrame();
        // Emissão e sincronização dos quadros
        Graphics::NativeRenderer::EndFrame();
        frameCount++;
    }

    Graphics::NativeRenderer::Shutdown();
    Kernel::MemoryManager::Shutdown();

    std::cout << "[Main] Execução finalizada com sucesso." << std::endl;
    return 0;
}
