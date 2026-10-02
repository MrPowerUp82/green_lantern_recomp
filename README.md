# Green Lantern: Rise of the Manhunters - Native PC Recomp

Projeto de recompilação estática nativa (Static Binary Recompilation) para PC (Windows x64) do jogo **Green Lantern: Rise of the Manhunters** (Xbox 360), utilizando:
* **XenonRecomp** para recompilação estática das instruções Xenon PowerPC para C++.
* **XenosRecomp** para tradução de shaders Xbox 360 para HLSL / SPIR-V.
* **Plume RHI** para renderização nativa direta em Vulkan 1.2+ / Direct3D 12 (sem emulador de GPU).
* **HLE Kernel Layer** para emulação em alto nível de `xboxkrnl.exe` e `xam.xex`.

## Estrutura do Projeto

```text
├── game/                 # Assets originais e executável default.xex
├── tools/                # Scripts de análise, unpack de XEX e helpers
├── src/
│   ├── kernel/           # Implementação HLE de chamadas de SO (threads, memória, I/O)
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
