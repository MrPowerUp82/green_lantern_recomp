# Renderer nativo Vulkan — Sub-projetos 2 a 5: draws, shaders, EDRAM e frontbuffer

Data: 2026-10-26
Status: implementado e validado com testes sintéticos e com os shaders do executável. Não validado
com o jogo rodando, porque o executável principal ainda não compila (seção 8).

## 1. Contexto

O M1 (spec de 2026-10-05) entregou o ring PM4, os imports `Vd*`, interrupções e o `XE_SWAP`
apresentando uma cor de limpeza. Os draws eram só contados. Este trabalho cobre os sub-projetos
restantes daquele spec:

2. Estado e recursos: texturas, vertex/index buffers, constantes.
3. Shaders: microcódigo Xenos → SPIR-V.
4. Render targets, resolve e EDRAM.
5. Apresentação do frontbuffer real.

A abordagem continua sendo emular o PM4 (como o Xenia), não interceptar funções do D3D. As
fórmulas e a semântica vêm do [Xenia Canary](https://github.com/xenia-canary/xenia-canary)
(revisão `291f0c9`, 2026-10-25), lido como referência. Os headers de formatos foram portados sem
alterações de lógica para `src/gpu/xenos/xenia/gpu/` (licença BSD, `src/gpu/xenos/LICENSE.xenia`),
com um shim mínimo de `xenia/base` em `src/gpu/xenos/xenia/base/`.

## 2. Arquitetura

| Unidade | Responsabilidade | Depende de |
|---|---|---|
| `XenosFormats` (`src/gpu/xenos/`) | `xenos.h`, `ucode.h`, `registers.h`, `register_table.inc`, `texture_info*`, `texture_util`, `texture_address.h` do Xenia | nada |
| `GpuCore` (`src/gpu/`) | CP: `IM_LOAD*`, `DRAW_INDX*`, resolves, `EVENT_WRITE_ZPD`, predicado de bins; `DrawCall`/`RegisterFileView` (`draw_call.h`); cena do selftest | `XenosFormats` |
| `XenosShader` (`src/gpu/shader/glsl_translator.*`) | microcódigo → GLSL 4.50; layout das constantes por draw (`draw_constants.h`); montador de microcódigo para testes (`ucode_builder.h`) | `XenosFormats` |
| `ShaderCompiler` (`src/gpu/shader/spirv_compiler.*`) | GLSL → SPIR-V com glslang 15.1.0 | glslang |
| `XenosDraw` (`src/gpu/draw/`) | índices e topologia; estado de render (viewport, scissor, NDC, blend, depth/stencil, culling, constantes de sistema); resolve; leitura de texturas | `GpuCore` |
| `Renderer` (`src/graphics/`) | `XenosRenderer`: execução no Plume (Vulkan); `NativeRenderer`: janela, swapchain e apresentação | todos acima, Plume |

`XenosDraw` e `XenosShader` não conhecem a API gráfica e têm testes próprios. O CP continua sem
Plume: entrega `IssueDraw(DrawCall)` e `IssueCopy(RegisterFileView)` ao `IGpuBackend`.

## 3. Fluxo de um draw

1. O CP decodifica `DRAW_INDX` (token viz, `VGT_DRAW_INITIATOR`, `VGT_DMA_*`) e chama
   `IssueDraw`, ou `IssueCopy` se `RB_MODECONTROL.edram_mode == kCopy`. Draws com
   `kNoOperation` são ignorados.
2. `ProcessPrimitive` lê os índices (endian, 24 bits, reset → `0xFFFFFFFF`) e converte o que o
   host não desenha: quad lists/strips → triângulos, line loop → linhas, leques com reset →
   triângulos. Rect lists viram 6 vértices por retângulo e o VS lê os índices do guest.
3. Os shaders ativos são traduzidos uma vez por hash (o VS tem uma variante para rect lists).
4. `ComputeRenderState` deriva o estado dos registradores (portado de `draw_util.cc`).
5. `XenosRenderer` obtém render targets, pipeline, texturas e samplers; grava no buffer de upload o
   bloco de constantes (512 vec4 float, bools, loops, fetch constants, remapeamento dos vertex
   fetches, constantes de sistema), os dados de vértice usados e os índices; e desenha.

## 4. Shaders

* Um laço `while`/`switch` sobre os índices de controle de fluxo implementa exec condicional
  (bool constant ou predicado), `loop_start`/`loop_end` com `aL`, `call`/`return` e `jmp`.
* Todas as 30 operações vetoriais e 50 escalares seguem o interpretador do Xenia, incluindo a regra
  do D3D9 (0 × qualquer coisa = 0), `max`/`min` com desempate, `setp_*`, `kill*`, `maxa`/`maxas`,
  `*_prev` e as variantes que limitam infinitos.
* Operandos: swizzle, negação, `abs`, endereçamento relativo por `a0` e `aL`; resultados com
  máscara, clamp, exports e escritas de constantes 0/1.
* `vfetch` lê o buffer de upload com troca de endian e os 15 formatos de vértice; `tfetch` cobre 1D,
  2D, 3D e empilhada (escolhida pela dimensão da fetch constant), cubo, coordenadas não
  normalizadas, offsets, LOD implícito/registrador/bias, gradientes, `getWeights`, `getCompTexLOD`,
  sinal (`kGamma` com a curva PWL do Xenos, `kUnsignedBiased`), swizzle e `exp_adjust`.
* VS: transforma a posição como o Xenia (`PA_CL_VTE_CNTL` e NDC). PS: interpoladores nos
  registradores, `ps_param_gen`, alpha test, `color_exp_bias`, gamma PWL para RTs `_GAMMA`, `oDepth`.

## 5. EDRAM, resolves e apresentação

* Cada combinação (base no EDRAM, pitch, formato do host) vira um render target do host de
  `pitch × 2048`. Formatos sem equivalente no Plume usam um formato maior
  (`2_10_10_10` → RGBA16, `16_16` com sinal → float16). Profundidade usa `D32_FLOAT_S8_UINT`.
* Resolve (`GetResolveInfo` do Xenia): o retângulo vem dos 3 vértices na fetch constant 0. A cópia
  vai para uma textura do host associada ao `RB_COPY_DEST_BASE`. Texturas lidas depois naquele
  endereço usam essa cópia. Profundidade é copiada para R32F por um passe de tela cheia.
* Clears (`color_clear_enable`/`depth_clear_enable`) usam `RB_COLOR_CLEAR[_LO]`/`RB_DEPTH_CLEAR`
  convertidos por formato (mesma tabela do Xenia).
* `XE_SWAP`: o frontbuffer é a textura resolvida no endereço do swap, desenhada na swapchain.

## 6. Texturas

`DescribeTexture` lê a fetch constant (dimensões, faixa de mips, endereços sem a máscara de 512 MB
do Xenia — ver seção 8). `DecodeTexture` usa `GetGuestTextureLayout`, `Tiled2D`/`Tiled3D` e
`GetPackedMipOffset` do Xenia, troca o endian por palavra de 32 bits e converte o formato:
BC1–BC5 passam direto; formatos de 16 bits por texel viram RGBA8; `10_11_11`/`11_11_10`/`2_10_10_10`
viram RGBA16; `24_8`/`24_8_FLOAT` viram R32F; `DXT3A`, `CTX1` e YUV são decodificados na CPU.
O cache valida o conteúdo por hash uma vez por quadro.

## 7. Verificação

* `gpu_tests` (93 casos): CP (IM_LOAD, draws, copias, ZPD, predicado), índices, estado de render,
  clears, resolve, texturas (linear com endian, tiled comparado com `Tiled2D`, 5_6_5, DXT1, hash) e
  a cena do selftest com backend nulo.
* `gpu_shader_tests` (6 casos): programas montados à mão (VS, rect list, PS com textura, laços,
  saltos, predicados e kill) e, com `GREEN_LANTERN_SHADER_CORPUS`, os 192 shaders do
  `default.xex` (160 PS, 32 VS) traduzidos e compilados para SPIR-V sem falhas nem instruções
  sem suporte.
* `gpu_selftest`: cena completa pelo ring PM4, com leitura de pixels e profundidades
  (`docs/superpowers/validation-native-renderer-2026-10-26.md`).

## 8. Limitações conhecidas

* **Não testado com o jogo.** O executável principal continua bloqueado pelo código recompilado
  (spec do M1, seção 10) e pelo kernel HLE. `src/main.cpp` já liga o renderer
  (`AttachGuestMemory`) e compila isoladamente.
* **Endereços da GPU = endereços virtuais do guest.** O port usa `MmGetPhysicalAddress` como
  identidade. Se o kernel passar a alocar memória física como no 360 (0xA0000000+ com o físico nos
  29 bits baixos), as leituras da GPU precisam traduzir o endereço. Os pontos de leitura são
  `GuestMemory` e `DescribeTexture`.
* Shaders: memexport (`eA`/`eM`) é ignorado; os shaders do corpus do executável têm `vfetch` sem o
  formato, que o D3D preenche em tempo de execução ao ligar a declaração de vértices.
* EDRAM: sem aliasing entre formatos ou bases sobrepostas, e sem MSAA (2x/4x são desenhados em 1x).
* Resolve: não grava os dados na memória do guest (leituras pela CPU veem dados antigos). Resolves
  com offset no destino só funcionam se as coordenadas coincidirem com as do render target.
* Plume: sem constantes de blend (fatores constantes usam zero), e o stencil de trás usa a
  referência e as máscaras da frente.
* Texturas atualizadas pela CPU no meio de um quadro só são vistas no quadro seguinte. Vértices e
  índices são copiados a cada draw (sem cache).
* RTs `_GAMMA` fazem blend no espaço gamma. O gamma ramp do display (`DC_LUT_*`) não é aplicado.
* O fallback Direct3D 12 não desenha (os shaders traduzidos são SPIR-V).
