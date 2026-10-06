# Validacao do default.xex fornecido em 2026-10-06

A implementacao PM4/Vulkan esta no commit local 1b09f46. Esta verificacao acrescenta os fontes de CPU gerados a partir do executavel fornecido pelo usuario, sem alterar o escopo do plano M1.

## Executavel e geracao

- default.xex: 9.023.488 bytes; SHA256 `3218a70a78f2d258cd236c204ddbca0cee21c197d6de463f68f6e504385852d1`.
- PE descompactado: 14.680.064 bytes; base 0x82000000; entrada 0x822FC750.
- XenonRecomp: revisao `ddd128bcca99fe8bfbb99bea583c972351fa6ace`, compilado com Clang 18.1.3.
- Geracao: config.toml existente, com caminhos relativos ajustados num arquivo temporario. Os headers ppc_config.h e ppc_recomp_shared.h gerados conferem com os rastreados.
- O gerador foi compilado com `-UXENON_RECOMP_USE_ALIAS`, emitindo wrappers em vez do atributo alias. Isso remove essa dependencia de compilador; nao demonstra compatibilidade integral com MSVC.
- Saida: 139 unidades ppc_recomp.*.cpp e ppc_func_mapping.cpp. Os arquivos gerados continuam ignorados pelo git e estao incluidos no ZIP de entrega.
- Cobertura estrutural: 35.706 simbolos do mapping encontrados nos fontes recompilados ou nos exports/stubs. Nao equivale a teste de link nem a validacao funcional das instrucoes.
- Imports: 219; 21 implementados e 198 stubs. Os stubs ainda nao implementam os servicos necessarios para executar o jogo completo.
- A execucao final do gerador terminou com codigo 0, mas registrou 2.986 ocorrencias de `Unrecognized instruction`. E necessario investigar esses avisos antes de afirmar fidelidade da recompilacao; o log bruto nao integra a entrega.

## Compilacao e limites

A verificacao sintatica usa Clang 18.1.3, C++20, AVX2 e FMA, nas 140 unidades C++. Os diagnosticos completos constam em validation/generated-syntax-check.json no ZIP.

O bloqueio conhecido do plano permanece: ppc_recomp.89.cpp referencia loc_82574740 sem label na funcao, e ppc_recomp.90.cpp referencia loc_8257479C. Esses saltos de CPU nao foram corrigidos neste trabalho. O plano exclui explicitamente os problemas preexistentes de recompilacao.

GpuCore continua compilando; os 66 testes GPU passam. A validacao inicial tambem passou em UBSan e compilou o renderer Plume em Linux. Nao houve compilacao integral com MSVC, link do jogo, teste visual Vulkan ou execucao dos assets do jogo.

## Uso da entrega

O ZIP atualizado contem os fontes rastreados da implementacao e os fontes C++ recompilados. Dependencias externas nao estao vendorizadas: inicialize XenonRecomp e Plume conforme README.md. O ZIP nao inclui o executavel original nem os assets.

Copie seu default.xex para game/default.xex e gere a imagem de runtime com:

```sh
python tools/unpack_xex.py game/default.xex game/default_unpacked.pe
```

Para os testes isolados, configure `-DGREEN_LANTERN_GPU_CORE_ONLY=ON`; se necessario, informe `-DSIMDE_INCLUDE_DIR=<pasta com x86/avx.h>`. Compile e execute CTest. O executavel completo continua bloqueado pelos erros de CPU acima. Os fontes gerados foram disponibilizados para revisao, nao como um build jogavel.
