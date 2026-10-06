# Renderer nativo Vulkan — Sub-projeto 1: Command Processor PM4 + Present

Data: 2026-10-05
Status: design aprovado. Emendado em 2026-10-06 durante o plano de implementação (ver "Emendas" no fim).

## 1. Contexto

O projeto recompila *Green Lantern: Rise of the Manhunters* (Xbox 360) para PC com XenonRecomp. O código recompilado chama a biblioteca D3D9 do Xbox 360, linkada estaticamente e sem símbolos. Essa biblioteca fala com a GPU escrevendo pacotes PM4 num ring buffer, via os imports `Vd*` do kernel.

Estado atual do repositório:

- `src/graphics/renderer.{h,cpp}` já tem um renderer sobre **Plume** (Vulkan, com fallback D3D12): janela Win32, swapchain, clear de cor e present. Ele ainda não desenha nada vindo do jogo. As alterações estão no working tree, não commitadas.
- O jogo roda numa thread guest (`_xstart`, via `src/kernel/guest_thread.cpp`).
- O kernel HLE é mínimo. `ExportManager` (ordinais com `std::function`) não está ligado ao código recompilado e fica sem uso.
- O código recompilado declara 219 `PPC_EXTERN_FUNC(__imp__*)`, e **nenhum tem definição no host**. O executável não linka hoje. Desses imports, 20 são `Vd*`.
- `ppc_context.h` define `__attribute__(x)` como vazio no MSVC, então `PPC_WEAK_FUNC` não funciona nesse compilador.
- O `MemoryManager` reserva 4 GB e commita só as regiões do título, da tabela de despacho, da stack e de 256 MB de heap. Alocações usam um bump allocator em `0x40000000`.

## 2. Decomposição do renderer nativo

O renderer completo é grande demais para um único spec. Os sub-projetos, cada um com seu ciclo spec → plano → implementação:

1. **Captura de comandos da GPU** (este spec): imports `Vd*`, command processor PM4, present.
2. Estado e recursos: texturas (detile, endian), vertex/index buffers, constantes.
3. Shaders: microcódigo Xenos → SPIR-V.
4. Render targets, resolve e EDRAM.
5. Apresentação do frontbuffer real.

Decisão de abordagem (sub-projeto 1): **emulação de PM4 estilo Xenia**, não hook de funções D3D9. O motivo é que a D3D do 360 está sem símbolos e boa parte dela é inlinada no código do jogo. Emular PM4 não exige localizar funções.

## 3. Objetivo e critério de sucesso (M1)

O M1 é "**PM4 + Present**":

- Os 20 imports `Vd*` funcionam.
- Um command processor decodifica pacotes PM4 de um ring buffer, mantém o register file e atualiza o RPTR.
- `XE_SWAP` produz um frame na swapchain Vulkan.
- O callback de interrupção gráfica é disparado (vblank e pacote `INTERRUPT`).
- O executável principal compila os fontes novos e tem definição para todos os 219 `__imp__*`.

Critério: a suíte `gpu_tests` passa, e a verificação manual com `gpu_selftest` (seção 9) mostra a janela Vulkan apresentando os frames do ring PM4. O link e a execução do executável principal dependem de um bloqueio pré-existente fora deste spec (seção 10).

Fora do escopo do M1: draws, shaders, texturas, EDRAM e resolve, áudio, kernel HLE real. Fazer o jogo real chegar a `VdInitializeRingBuffer` e `VdSwap` é desejável, mas **não bloqueia** o M1, porque depende do kernel HLE (threads, TLS, eventos).

## 4. Arquitetura

| Unidade | Responsabilidade | Depende de |
|---|---|---|
| `src/kernel/vd_exports.cpp` | Os 20 `__imp__Vd*` como `PPC_FUNC`. Guardam estado em `GpuState`. | `GpuState`, memória do guest |
| `src/kernel/import_stubs.cpp` (gerado por `tools/gen_import_stubs.py`) | Stubs que logam e retornam 0 para os ~199 imports não-`Vd`. O script lê `src/recompiled/ppc_recomp_shared.h` e pula os nomes que têm implementação real. Não usa weak (ver seção 1). | `ppc_recomp_shared.h` |
| `src/gpu/command_processor.{h,cpp}` | Thread do CP: polling do WPTR, leitura do ring e dos indirect buffers, decodificação PM4, register file, write-back do RPTR. | `IGpuBackend`, `base` do guest (parâmetro) |
| `src/gpu/gpu_registers.h` | Índices de registradores usados pelo M1. | nada |
| `src/gpu/gpu_backend.h` | Interface `IGpuBackend` (`Swap`, `OnResize`) e `NullBackend` para testes. | nada |
| `src/graphics/renderer.cpp` (existente) | Implementa `IGpuBackend` com Plume. O loop atual de `BeginFrame`/`EndFrame` na main thread é substituído por acquire, clear e present disparados por `XE_SWAP`. | Plume |
| `src/gpu/interrupts.{h,cpp}` | Thread de vblank a 60 Hz e despacho do callback guest, via um `GuestCaller` injetado (fake nos testes). | nada |
| `src/kernel/guest_call.{h,cpp}` | `CreateGuestCaller`: executa uma função guest num `PPCContext` com PCR e stack próprios (usado pelo callback de interrupção). | `ppc_context.h`, `guest_thread.h` |
| `src/gpu/gpu_system.{h,cpp}` | Estado global (`GpuSystem::Get()`) usado pelos `Vd*`: CP, dispatcher, página MMIO. | CP, interrupções |
| `src/gpu/selftest.{h,cpp}` | `RunSelfTest`: simula o D3D (chama os `Vd*` e publica swaps no ring). Base do teste de integração e do `gpu_selftest`. | `GpuSystem`, `Vd*` |
| `src/gpu_selftest/main.cpp` | Executável `gpu_selftest`: janela Plume + CP + `Vd*` sem o jogo e sem o código recompilado. | `GpuCore`, Plume |
| `src/kernel/mm_exports.cpp` | `__imp__MmGetPhysicalAddress` real (identidade: este port não separa físico de virtual). | nada |

`CommandProcessor` não conhece Plume. Recebe `base` e um `IGpuBackend*` no construtor, o que permite testá-lo sem GPU nem singleton.

## 5. Fluxo de inicialização (`Vd*`)

- `VdInitializeRingBuffer(base, size_log2)`: guarda o ring e inicia a thread do CP.
- `VdEnableRingBufferRPtrWriteBack`: guarda o endereço de write-back do RPTR.
- `VdSetSystemCommandBufferGpuIdentifierAddress`: guarda o endereço, sem uso no M1. O Xenia o trata como no-op; os fences chegam ao jogo por `EVENT_WRITE_SHD` no stream de comandos.
- `VdSetGraphicsInterruptCallback`: guarda o endereço do callback guest e seu argumento de usuário.
- `VdQueryVideoMode`, `VdGetCurrentDisplayInformation`: preenchem as structs em big-endian com 1280x720.
- `VdGetSystemCommandBuffer`: retorna um buffer de sistema pequeno, alocado uma vez.
- `VdSwap`: monta um pacote `XE_SWAP` no command buffer recebido do jogo.
- `VdRetrainEDRAM*`, `VdEnableDisableClockGating`, `VdShutdownEngines`, `VdInitializeEngines`, `VdPersistDisplay`, `VdSetDisplayMode`, `VdIsHSIOTrainingSucceeded`, `VdInitializeScalerCommandBuffer`, `VdQueryVideoFlags`, `VdGetCurrentDisplayGamma`, `VdCallGraphicsNotificationRoutines`: fazem só o necessário para o jogo seguir (retorno de sucesso e structs mínimas).

`MmGetPhysicalAddress` (chamado pelo jogo antes de `VdInitializeRingBuffer`) passa a ser a identidade, e o alocador do `MemoryManager` passa a ser atômico (guest, CP e thread de interrupção alocam concorrentemente).

O `MemoryManager` passa a commitar a página de registradores da GPU em `0x7FC80000` (64 KB). Os registradores que o D3D lê dali (`0x0F00`, `0x0F01`, `0x194C`, `0x1951`, `0x1961`) são inicializados com os valores do Xenia, em big-endian, porque o código recompilado lê a página com loads big-endian. Os índices exatos (`CP_RB_WPTR` e os demais) são conferidos contra as tabelas de registradores do Xenia durante a implementação e fixados em `gpu_registers.h`.

## 6. Loop do command processor

1. Lê o WPTR da página de registradores. Se `rptr == wptr`, espera de forma adaptativa (spin, `yield`, sleep de ~100 µs).
2. Lê dwords big-endian do ring, com wraparound por potência de 2, e decodifica o header:
   - Tipo 0: escrita de registradores, com a flag `one_reg`.
   - Tipo 2: filler.
   - Tipo 3: opcode, contagem e predicado.
3. Executa o pacote, avança o RPTR e faz o write-back no endereço registrado, com barreira de memória.

Opcodes do M1:

- **Tratados de verdade:** `NOP`, `INTERRUPT`, `EVENT_WRITE`, `EVENT_WRITE_SHD`, `EVENT_WRITE_EXT` (escrevem o fence na memória), `WAIT_REG_MEM`, `REG_RMW`, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, `INDIRECT_BUFFER` (e a variante PFD), `ME_INIT`, `XE_SWAP`.
- **Só gravam no register file:** `SET_CONSTANT*`, `LOAD_ALU_CONSTANT`.
- **Contados e logados, sem executar:** `DRAW_INDX*` e demais opcodes.

`XE_SWAP` chama `backend->Swap(frontbufferAddr, width, height)` e incrementa o contador de frames.

## 7. Threads e sincronização

- A thread do CP é dona do device Plume. `Swap()` faz acquire, clear e present nela.
- A thread principal só roda `PollEvents` e espera. `WM_SIZE` ativa uma flag atômica, e o CP recria a swapchain no próximo `Swap`.
- A thread de vblank (60 Hz) e o pacote `INTERRUPT` disparam o callback guest. O callback roda numa thread host própria, com `PPCContext` e PCR próprios. Só é disparado depois de registrado.
- Os ponteiros do ring são atômicos. O estado compartilhado entre os `Vd*` (thread guest) e o CP fica em `GpuState`.
- Shutdown: flag `stop` atômica, `join` do CP e da thread de vblank, e só então a destruição do device.

## 8. Tratamento de erros

- **Pacote malformado** (tipo 1, contagem que passa do ring, endereço nulo ou fora de 4 GB): loga o offset uma vez, descarta até o WPTR e incrementa um contador. O CP nunca trava o jogo.
- **Falha no `Swap`** (device lost): loga e segue consumindo o ring. O fallback D3D12 já existe na inicialização do renderer.
- **Opcode desconhecido:** pula pelo tamanho declarado e conta, sem perder o sincronismo do ring.

## 9. Testes

Novo executável `gpu_tests`, registrado no CTest, com macros de assert mínimas (sem framework novo). O `CommandProcessor` recebe `base` por parâmetro e usa `NullBackend`. A memória do guest vem do próprio `MemoryManager`, que já reserva 4 GB.

Casos:

- Decodificação de headers dos tipos 0, 2 e 3.
- Tipo 0 com `one_reg` e com wraparound do ring.
- Opcode desconhecido é pulado sem perder o sincronismo.
- Write-back do RPTR.
- `EVENT_WRITE_SHD` escreve o fence em big-endian.
- `WAIT_REG_MEM` completa quando o valor chega.
- `INDIRECT_BUFFER` aninhado.
- `XE_SWAP` chama `NullBackend::Swap` com os parâmetros certos.
- `VdSwap` emite o pacote correto.
- Layout big-endian de `VdQueryVideoMode`.
- Pacote malformado descarta até o WPTR e o CP continua vivo.

Verificação manual (não faz parte do CTest): `gpu_selftest.exe [frames]` abre a janela Vulkan, publica os swaps pelo ring PM4 e termina com `OK ... malformed=0`. Já executado numa Intel UHD durante o planejamento (120 swaps, 6480 pacotes, 0 malformados). Quando o código recompilado compilar, rodar o executável principal deve mostrar os logs das chamadas `Vd*` e dos imports não implementados.

## 10. Riscos

- **Bloqueio pré-existente, fora deste spec:** `RecompiledCode` não compila com o MSVC `cl` desta máquina (244 usos de `__sync_bool_compare_and_swap`, 63 de `__builtin_debugtrap`, 13 de `__builtin_clzll`, e dois `goto` para rótulos que o gerador deixou em outra função, `loc_82574740` e `loc_8257479C` em `ppc_recomp.89/90.cpp`). Não há `clang-cl` instalado. Enquanto isso não for resolvido, o executável principal não linka, com ou sem a GPU. Os 219 imports e os demais símbolos do mapeamento foram conferidos estaticamente: depois que o código recompilado compilar, os únicos símbolos a resolver são os `__imp__*`, todos cobertos. Recomendo tratar isso como um spec próprio.
- Os índices de registradores e os formatos dos pacotes `XE_SWAP` e `VdSwap` vêm da referência do Xenia e precisam ser conferidos na implementação. Nenhum deles pode ser inventado.
- O callback de interrupção é função guest e pode exigir comportamento de kernel que hoje é stub. Se isso causar problema, o disparo fica atrás de uma flag e o M1 ainda é validado pelos testes sintéticos.
- O ring do jogo real vem de `MmAllocatePhysicalMemoryEx`, hoje stub. No M1, os testes fornecem o ring diretamente.

## Emendas (2026-10-06)

Feitas ao escrever o plano, depois de conferir o código do Xenia e compilar um protótipo:

- `VdSetSystemCommandBufferGpuIdentifierAddress` só guarda o endereço (o spec dizia que o CP publicava o fence nele; o Xenia não faz isso).
- `LOAD_ALU_CONSTANT` não mascara o endereço, porque físico == virtual neste port.
- `WAIT_REG_MEM` em registrador avalia a condição uma vez e segue (só o CP altera o register file, então esperar travaria para sempre). Em memória espera até a condição valer ou até `Stop()`.
- Adicionados `gpu_selftest`, `GpuSystem`, `guest_call` e `mm_exports` (seção 4).
- O critério do M1 passou a usar `gpu_selftest` em vez do executável principal, pelo bloqueio da seção 10.
