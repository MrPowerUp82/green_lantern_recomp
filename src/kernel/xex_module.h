#pragma once

#include "types.h"

#include <filesystem>

namespace Kernel {

    // Emula o loader do XEX para o modulo do jogo:
    //  - copia o cabecalho do XEX para o guest (RtlImageXexHeaderField le campos dele);
    //  - cria o LDR_DATA_TABLE_ENTRY do modulo (XexExecutableModuleHandle);
    //  - preenche a IAT: nos registros de variaveis grava o endereco da variavel exportada pelo kernel
    //    (KeTimeStampBundle, XboxHardwareInfo, ...), nos de funcao o endereco do thunk;
    //  - inicia a thread que atualiza KeTimeStampBundle.
    // importsFile e gerado por tools/dump_xex_image junto com default.image.
    bool LoadXexModule(const std::filesystem::path& xexFile, const std::filesystem::path& importsFile);

    // Ponteiro (no guest) para o valor/dado do campo opcional `key` do cabecalho, ou 0.
    GuestAddr XexHeaderField(uint32_t key);

    // LDR_DATA_TABLE_ENTRY do modulo do jogo.
    GuestAddr XexModuleHandle();

    // Para a thread de relogio (chamado no encerramento).
    void ShutdownXexModule();
}
