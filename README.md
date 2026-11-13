# Green Lantern: Rise of the Manhunters - Native PC Recomp

Projeto de recompilação estática nativa (Static Binary Recompilation) para PC (Windows x64) do jogo **Green Lantern: Rise of the Manhunters** (Xbox 360), utilizando:
* **XenonRecomp** para recompilação estática das instruções Xenon PowerPC para C++.
* Um **tradutor de microcódigo Xenos → GLSL → SPIR-V** em tempo de execução (semântica do Xenia Canary).
* **Plume RHI** para o renderer nativo em Vulkan (draws, render targets do EDRAM, texturas e resolves) e
  Direct3D 12 (só apresentação), alimentado por um command processor PM4 do Xenos.
* **HLE Kernel Layer** para emulação em alto nível de `xboxkrnl.exe` e `xam.xex`.

## Estrutura do Projeto

```text
├── game/                 # Assets originais e executável default.xex
├── tools/                # Scripts de análise, unpack de XEX e helpers
├── src/
│   ├── kernel/           # Implementação HLE de chamadas de SO (threads, memória, I/O)
│   ├── gpu/              # Command processor PM4, interrupcoes, selftest, tradutor de shaders
│   │                     #   (shader/), preparacao de draws (draw/) e formatos do Xenia (xenos/)
│   ├── gpu_selftest/     # Verificacao manual com janela, sem o jogo
│   ├── graphics/         # Backend de renderização nativo (Plume / Vulkan / D3D12)
│   ├── audio/            # Backend de áudio (wrapper nativo FMOD Ex / XAudio2)
│   ├── platform/         # Janelamento, entrada de controles (SDL3/XInput) e filesystem
│   └── recompiled/       # Unidades de compilação C++ geradas pelo XenonRecomp
├── include/              # Headers comuns e definições de tipos
└── CMakeLists.txt        # Configuração de build para MSVC / Clang-cl
```

## Requisitos de Construção
* Visual Studio 2022 (MSVC v143 ou Clang-cl) com suporte a C++20 e AVX2
* CMake 3.28+
* Ninja (opcional, para builds ultra-rápidos)
* GPU compatível com Vulkan 1.2 ou DirectX 12 (Shader Model 6)

## Gerando e compilando o jogo (Windows / MSVC)

O jogo precisa do `game/default.xex` e dos assets em `game/` (nao versionados). Os passos abaixo geram o
codigo recompilado e o executavel; todos os caminhos sao relativos a raiz do repositorio.

```powershell
# 1. XenonRecomp (gerador) e as ferramentas de apoio
git clone --recurse-submodules https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G "Visual Studio 18 2026" -A x64
cmake --build tools/XenonRecomp/build --config Release --target XenonRecomp XenonAnalyse
cmake -S tools/dump_xex_image -B build/dump_xex_image
cmake --build build/dump_xex_image --config Release

# 2. Imagem descompactada do XEX + tabela de imports (game/default.image e default.image.imports.txt)
build/dump_xex_image/Release/dump_xex_image.exe game/default.xex game/default.image

# 3. Tabelas de switch (tools/jump_table.toml) e geracao do codigo C++ (src/recompiled/ppc_recomp.*.cpp)
python tools/find_jump_tables.py game/default.image tools/jump_table.toml
tools/XenonRecomp/build/XenonRecomp/Release/XenonRecomp.exe config.toml tools/ppc_context.h
python tools/find_branch_targets.py      # declara em config.toml as entradas de funcao que faltam
tools/XenonRecomp/build/XenonRecomp/Release/XenonRecomp.exe config.toml tools/ppc_context.h
# repita find_branch_targets.py + XenonRecomp ate nao restar "// ERROR" em src/recompiled

# 4. Executavel
cmake -S . -B build -DGREEN_LANTERN_BUILD_GAME=ON
cmake --build build --config Release --target GreenLanternRecomp
build/bin/Release/GreenLanternRecomp.exe
```

Observacoes sobre as ferramentas:

* O XenonRecomp precisa de pequenos patches locais (nao ha clang no VS Build Tools): `byteswap.h` para MSVC,
  instrucoes que faltavam (formas "update" `lhzu`/`sthu`/`lfsu`/`stfsu`, `addc`, `subfze`, `eqv`, `vsel128`,
  `bdzf`, ...), branches de CTR para fora da funcao e `mftb` com o relogio de 50 MHz do Xbox 360
  (`ppc_get_timebase`). Eles ficam em `tools/XenonRecomp` (repositorio aninhado, ignorado pelo Git).
* O gerador sobrescreve `src/recompiled/ppc_context.h` com o arquivo passado como segundo argumento. Use
  `tools/ppc_context.h`, que ja traz os shims de MSVC (`__builtin_*`, `__sync_bool_compare_and_swap`).
* As unidades com funcoes gigantes (>= 5000 linhas) compilam sem otimizacao (`slow_units.cmake`); com `/O2`
  o MSVC leva horas nelas.
* `GL_TRACE_KERNEL=1` imprime cada chamada ao kernel (nome, argumentos e retorno). Em falhas do host o
  executavel mostra o endereco acessado, os registradores e a pilha do guest.
* Sem controle XInput, o teclado simula o controle 0: WASD = analogico esquerdo, setas = direito, Espaco = A,
  E = B, Q = X, R = Y, 1/3 = LB/RB, Shift = LT, Z = RT, Enter = Start, Backspace = Back, Ctrl esq. = L3, C = R3,
  IJKL = direcional.

## Renderer nativo

A GPU Xenos é emulada no nível do PM4 (o D3D do 360 está inlinado no jogo e sem símbolos):

* **Command processor** (`src/gpu/command_processor.*`): ring e indirect buffers, registradores,
  `IM_LOAD`/`IM_LOAD_IMMEDIATE`, `DRAW_INDX`/`DRAW_INDX_2`, resolves (`RB_MODECONTROL == kCopy`),
  consultas de oclusão simuladas (`EVENT_WRITE_ZPD`), predicado de bins e `XE_SWAP`.
* **Tradutor de shaders** (`src/gpu/shader/`): microcódigo Xenos → GLSL 4.50 → SPIR-V (glslang).
  Cobre controle de fluxo (exec condicional, loops, calls, jumps, predicados), todas as operações
  vetoriais e escalares da ALU, `vfetch` com todos os formatos de vértice e `tfetch` 1D/2D/3D/
  empilhado/cubo, com LOD, gradientes, offsets e sinal/gamma/swizzle da fetch constant.
* **Preparação de draws na CPU** (`src/gpu/draw/`): leitura e conversão de índices (reset de
  strips, quad lists, line loops, rect lists), viewport/scissor/NDC, blend, profundidade/stencil,
  culling, resolves e clears, e texturas (detiling, endian, mips empacotados, conversão de formato).
* **Execução no Vulkan** (`src/graphics/xenos_renderer.*`, sobre Plume): pipelines em cache,
  render targets do EDRAM, cache de texturas por conteúdo, resolves de cor e profundidade e
  apresentação do frontbuffer resolvido.

As fórmulas de registradores, endereçamento de texturas e semântica das instruções vêm do
[Xenia Canary](https://github.com/xenia-canary/xenia-canary): os headers de formatos estão
portados em `src/gpu/xenos/` (licença BSD em `src/gpu/xenos/LICENSE.xenia`). Detalhes, limitações
e decisões em `docs/superpowers/specs/2026-10-26-native-renderer-draws-design.md`.

Os draws exigem o backend Vulkan; no fallback Direct3D 12 a janela só apresenta a cor de limpeza.

## Testes e verificação da GPU

Os testes não dependem dos assets ou das unidades recompiladas do jogo. O
`gpu_selftest` também é independente de `RecompiledCode`, mas exige Windows
e as dependências Plume/SIMDe/glslang. Se essas dependências ainda não estiverem presentes:

```powershell
git clone --depth 1 https://github.com/simd-everywhere/simde.git thirdparty/simde
git clone --recurse-submodules https://github.com/renderbag/plume.git thirdparty/plume
git clone --depth 1 --branch 15.1.0 https://github.com/KhronosGroup/glslang.git thirdparty/glslang
```

O CMake encontra SIMDe em XenonRecomp ou `thirdparty/simde/simde`. Também aceita
`-DSIMDE_INCLUDE_DIR=<pasta que contém x86/avx.h>`.

```powershell
# Testes e janela sem compilar o jogo (independente dos bloqueios de RecompiledCode).
cmake -S . -B build -DGREEN_LANTERN_BUILD_GAME=OFF
cmake --build build --config Release --target gpu_tests gpu_shader_tests gpu_selftest -- /m
ctest --test-dir build -C Release --output-on-failure
.\build\bin\Release\gpu_selftest.exe 120 --screenshot frame.bmp
```

O `gpu_selftest` emite pelo ring PM4, como o D3D do 360, uma cena com microcódigo Xenos montado
à mão: clear de cor e profundidade por resolve, triângulo com textura tiled, triângulo atrás dele
(teste de profundidade), rect list e resolves de cor (frontbuffer) e de profundidade. No fim, lê de
volta o frontbuffer e a profundidade resolvidos e confere 7 pixels e 3 profundidades. Resultado
esperado: todas as linhas `OK`, `draws=360`, `copias=360`, `malformed=0` e saída 0.
Verifique também ESC, fechamento e redimensionamento da janela. `--swap-only` roda o teste do M1
(só swaps pelo ring).

Para traduzir e compilar todos os shaders do jogo no teste do tradutor, extraia o corpus do
executável (os arquivos ficam fora do Git):

```powershell
python tools/unpack_xex.py game/default.xex build/default.pe
python tools/extract_shaders.py build/default.pe build/shaders
$env:GREEN_LANTERN_SHADER_CORPUS = "build/shaders"
.\build\Release\gpu_shader_tests.exe
```

`GREEN_LANTERN_SHADER_DUMP=<pasta>` grava o GLSL gerado de cada shader do corpus.

Para compilar somente o núcleo e seus testes, sem Plume, use uma pasta separada:

```powershell
cmake -S . -B build/core -DGREEN_LANTERN_GPU_CORE_ONLY=ON
cmake --build build/core --config Release --target gpu_tests gpu_shader_tests
ctest --test-dir build/core -C Release --output-on-failure
```

O núcleo do M1 foi validado em Linux x64 (a memória de teste usa `mmap`/`mprotect`); as partes
novas (tradutor, preparação de draws) só foram compiladas no Windows.
`gpu_tests` tem 93 casos (CP, PM4, imports, interrupções, índices, estado de render, resolves,
texturas e a cena do selftest com backend nulo); `gpu_shader_tests` tem 6.

Para voltar a incluir o jogo, configure `-DGREEN_LANTERN_BUILD_GAME=ON`, com as
unidades `src/recompiled/ppc_recomp.*.cpp` e `ppc_func_mapping.cpp` geradas localmente.
Esses arquivos e as dependências não vêm neste repositório. Os bloqueios de MSVC
citados no plano (builtins e labels entre funções geradas) continuam fora do M1.
O argumento `--no-gpu-interrupts` desativa callbacks guest durante depuração.

Os outros imports são stubs gerados:

```powershell
python tools/gen_import_stubs.py
```

Rode o gerador novamente após implementar um `__imp__*` em `src/kernel/*_exports.cpp`.
São 219 imports: 21 implementados (20 `Vd*` e `MmGetPhysicalAddress`) e 198 stubs.
