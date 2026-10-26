# Validação do renderer nativo (draws) — 2026-10-26

Máquina: Windows 11, Visual Studio Build Tools 18 (MSVC 14.51), Intel UHD Graphics (Vulkan).
Dependências: Plume (HEAD de 2026-10-26), SIMDe, glslang 15.1.0. Referência: Xenia Canary `291f0c9`.

## Testes automatizados

| Alvo | Resultado |
|---|---|
| `gpu_tests` (`build`) | 93 casos, 0 falhas |
| `gpu_shader_tests` (`build`) | 6 casos, 0 falhas |
| `gpu_tests` + `gpu_shader_tests` (`build/core`, `GREEN_LANTERN_GPU_CORE_ONLY=ON`) | 2/2 no CTest |
| Corpus do `default.xex` (`GREEN_LANTERN_SHADER_CORPUS`) | 192 shaders (32 VS, 160 PS), 0 falhas de tradução ou de compilação SPIR-V, 0 instruções sem suporte |

O corpus foi gerado com `tools/unpack_xex.py` e `tools/extract_shaders.py` a partir do
`default.xex` de SHA256 `3218a70a…852d1`. Os VS também foram compilados na variante de rect list.

Dois testes antigos foram ajustados: `cp_draw_opcodes_are_counted_not_executed` (o pacote de draw
do M1 era inválido e agora é executado) e o teste de backpressure do selftest, que usava
`sleep_for(1ms)`, que dura ~15 ms no timer padrão do Windows e estourava o prazo de 2 s.

## gpu_selftest

`gpu_selftest.exe 30 --screenshot frame.bmp` (Vulkan, Intel UHD):

```
pixel (448,360): 255 0 0 255 esperado 255 0 0 255 OK      metade esquerda da textura (tiled)
pixel (832,360): 0 0 128 255 esperado 0 0 128 255 OK      metade direita * tinta do PS (c0)
pixel (100,650): 0 255 0 255 esperado 0 255 0 255 OK      triangulo de tras visivel
pixel (256,576): 255 0 0 255 esperado 255 0 0 255 OK      sobreposicao: o da frente vence
pixel (1270,10): 32 64 128 255 esperado 32 64 128 255 OK  clear de cor por resolve
pixel (160,80): 255 255 0 255 esperado 255 255 0 255 OK   rect list, vertices dados
pixel (256,144): 255 255 0 255 esperado 255 255 0 255 OK  rect list, 4o vertice calculado no VS
profundidade (256,576): 0.5 esperado 0.5 OK
profundidade (100,650): 0.7 esperado 0.7 OK
profundidade (1270,10): 1 esperado 1 OK                   clear de profundidade
Renderer: draws=90 pulados=0 resolves=90 clears=60 texturas=2 shaders=4 falhas_shader=0 pipelines=3 apresentacoes=30
OK: swaps=30 draws=90 copias=90 pacotes=3360 malformed=0
```

`gpu_selftest.exe 60 --swap-only` (teste do M1): `OK: swaps=60 ... malformed=0`.

A imagem salva mostra o triângulo texturizado (vermelho/azul), o triângulo verde parcialmente
oculto e o retângulo amarelo sobre o fundo do clear, apresentados a partir do frontbuffer resolvido.

## Não verificado

* O jogo: o executável principal não compila (código recompilado e kernel HLE). `src/main.cpp`
  com a chamada nova compila isoladamente com MSVC.
* Linux: as partes novas não foram compiladas fora do Windows.
* Validação do Vulkan (camadas de validação não instaladas nesta máquina) e outras GPUs.
* MSAA, aliasing do EDRAM, memexport e escrita do resolve na memória do guest (fora do escopo; ver
  as limitações no spec).
