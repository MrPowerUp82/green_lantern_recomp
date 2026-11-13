#pragma once

#include <cstdint>

namespace Platform {
    // Instala um handler que, em falhas do host (acesso invalido, instrucao ilegal, divisao por zero),
    // imprime o endereco acessado e um backtrace mapeado para funcoes do guest (sub_XXXXXXXX).
    void InstallCrashHandler(uint8_t* guestBase);

    // Publica o contexto PPC da thread atual para o relatorio de falhas (lr, registradores e pilha do guest).
    void SetCrashContext(void* ppcContext);
}
