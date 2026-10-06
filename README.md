# Green Lantern: Rise of the Manhunters - Native PC Recomp

Projeto de recompilação estática nativa (Static Binary Recompilation) para PC (Windows x64) do jogo **Green Lantern: Rise of the Manhunters** (Xbox 360), utilizando:
* **XenonRecomp** para recompilação estática das instruções Xenon PowerPC para C++.
* **XenosRecomp** para tradução de shaders Xbox 360 para HLSL / SPIR-V.
* **Plume RHI** para apresentação nativa em Vulkan / Direct3D 12, alimentada por um command processor PM4 do Xenos.
* **HLE Kernel Layer** para emulação em alto nível de `xboxkrnl.exe` e `xam.xex`.

## Estrutura do Projeto

```text
├── game/                 # Assets originais e executável default.xex
├── tools/                # Scripts de análise, unpack de XEX e helpers
├── src/
│   ├── kernel/           # Implementação HLE de chamadas de SO (threads, memória, I/O)
│   ├── gpu/              # Command processor PM4, interrupcoes e selftest
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

## Testes e verificação da GPU

O marco M1 implementa o ring PM4, os 20 imports `Vd*`, interrupções/vblank e
`XE_SWAP` → apresentação na swapchain. Draws são contados; shaders, texturas,
EDRAM e apresentação do frontbuffer real pertencem às próximas etapas.

Os testes não dependem dos assets ou das unidades recompiladas do jogo. O
`gpu_selftest` também é independente de `RecompiledCode`, mas exige Windows
e as dependências Plume/SIMDe. Se essas dependências ainda não estiverem presentes:

```powershell
git clone --depth 1 https://github.com/simd-everywhere/simde.git thirdparty/simde
git clone --recurse-submodules https://github.com/renderbag/plume.git thirdparty/plume
```

O CMake encontra SIMDe em XenonRecomp ou `thirdparty/simde/simde`. Também aceita
`-DSIMDE_INCLUDE_DIR=<pasta que contém x86/avx.h>`.

```powershell
# Testes e janela sem compilar o jogo (independente dos bloqueios de RecompiledCode).
cmake -S . -B build -DGREEN_LANTERN_BUILD_GAME=OFF
cmake --build build --config Release --target gpu_tests gpu_selftest -- /m
ctest --test-dir build -C Release --output-on-failure
.\build\bin\Release\gpu_selftest.exe 120
```

Resultado esperado da janela: backend Vulkan, `swaps=120`, `pacotes=6480`,
`malformed=0`. Verifique também ESC, fechamento e redimensionamento da janela.
O fallback para Direct3D 12 aparece no log e não valida o caminho Vulkan.

Para compilar somente o núcleo e seus testes, sem Plume, use uma pasta separada:

```powershell
cmake -S . -B build/core -DGREEN_LANTERN_GPU_CORE_ONLY=ON
cmake --build build/core --config Release --target gpu_tests
ctest --test-dir build/core -C Release --output-on-failure
```

Esse modo também funciona em Linux x64; a memória de teste usa `mmap`/`mprotect`.
A suíte possui 66 casos, incluindo wraparound, fences/endian, buffers indiretos,
imports, interrupções, parada durante WAIT e backpressure do selftest.

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
