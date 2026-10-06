# Implementação de PM4 + Present — 2026-10-06

Base: `21355bc` (main). Plano: `plans/2026-10-06-vulkan-gpu-command-processor.md`.

## Entregue

- `GpuCore` isolado de Plume, MemoryManager e RecompiledCode.
- Registradores MMIO, endian e headers PM4; leitor com wraparound e buffers indiretos.
- CP com thread/polling, RPTR, fences, WAIT, constantes, scratch, INTERRUPT e XE_SWAP.
- Despacho serial de callbacks e vblank; executor guest com stack/PCR próprios.
- 20 exports Vd*, MmGetPhysicalAddress identidade e 198 stubs gerados.
- Página MMIO e alocador atômico; main apresenta exclusivamente via CP.
- RendererBackend/PresentFrame com flags atômicas e encerramento que preserva a janela até o join.
- Selftest via imports e ring; antigo clear loop substituído.
- CMake/CTest, instruções de build e 66 testes.

## Adaptações ao estado do repositório

A tarefa 1 não se aplicava: os quatro arquivos do renderer já estavam commitados,
e o checkout estava limpo. A estrutura `src/CMakeLists.txt` já existia; foi
corrigida para manter o GpuCore independente. Acrescentadas opções
`GREEN_LANTERN_GPU_CORE_ONLY`, `GREEN_LANTERN_BUILD_GAME` e `SIMDE_INCLUDE_DIR`
para validar o núcleo e o selftest sem unidades recompiladas locais.

As unidades `ppc_recomp.*.cpp` e `ppc_func_mapping.cpp` estão ignoradas e não
vieram no clone. Não é possível verificar seus símbolos nem compilar/linkar o
executável principal completo neste checkout. Os headers PPC reais foram usados
nos testes e nas verificações de compilação; nenhum PPCContext fake foi criado.

Três regressões complementam os 63 casos do plano: stream com capacidade/índice
inválidos ou base desalinhada; Stop durante WAIT com intervalo máximo; produção
mais rápida que um backend lento, dando mais de duas voltas no ring. Esta última
falhou antes da correção (88 swaps observados para 600 publicados). O selftest
agora respeita espaço livre e confirma a contagem ao drenar.

Objetos de CP/interrupção parados são mantidos até a próxima inicialização para
imports que já passaram por Ready durante o encerramento. Inicialização é uma
operação de startup sem guest ativo; não oferece hot reload concorrente.

## Verificação executada

Ambiente: Linux x64, GCC 13.3, CMake 4.4.4.

- Release: 66 testes, zero falhas, CTest aprovado.
- Debug com UBSan (`-fsanitize=undefined -fno-sanitize-recover=all`): 66 testes,
  zero falhas e nenhum diagnóstico do sanitizer.
- Alvo Renderer compilou contra Plume real, incluindo a implementação Vulkan.
  Não houve janela ou dispositivo gráfico durante essa compilação.
- `main.cpp`, `memory.cpp`, `guest_call.cpp`, `import_stubs.cpp` compilaram
  individualmente com os headers PPC reais em Linux (não valida os ramos Win32).
- Cobertura estática: 219/219 imports, 21 implementações reais, 198 stubs, sem duplicatas.
- Gerador executado e `git diff --check` aprovado.

Referências primárias conferidas:

- Xenia `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`: `register_table.inc`,
  `graphics_system.cc`, `command_processor.cc`, `xboxkrnl_video.cc`.
  Conferidos índices, defaults MMIO, VdSwap, argumentos do scaler e extensões.
- Plume `d72379344dacd3dbf9f810f92ddc87e6de1845b1` e seus submódulos.
- SIMDe `0ff5341927e15df0c3da8bb13bb32ec7f95aef84`.

## Verificação pendente e limites

- Windows/MSVC e janela Vulkan: executar `gpu_selftest.exe 120`, conferir
  Vulkan, 120 swaps, 6480 pacotes e zero malformados; testar resize, ESC e fechar.
- Build principal: precisa das unidades recompiladas locais e continua sujeito
  aos bloqueios de MSVC já documentados no plano; não foram corrigidos neste M1.
- Jogo real: download do link Drive falhou por bloqueio de rede. A validação M1
  não depende dos assets, mas não houve execução do jogo.
- `graphify` indisponível: atualização do grafo não executada.
- O backend apresenta um clear; draws, shaders, texturas, EDRAM e frontbuffer real
  continuam fora do escopo deste plano.

O README contém os comandos para repetir os testes e a verificação manual.
