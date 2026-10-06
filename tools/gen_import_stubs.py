#!/usr/bin/env python3
"""Gera src/kernel/import_stubs.cpp.

O codigo recompilado declara cada import do kernel/XAM como `PPC_EXTERN_FUNC(__imp__Nome)`
em src/recompiled/ppc_recomp_shared.h. Cada um precisa de uma definicao no host para o
executavel linkar. Este script emite um stub que loga (uma vez) e retorna 0 para todo import
que ainda NAO tem implementacao real em src/kernel/*_exports.cpp.

Para implementar um import de verdade: defina `PPC_FUNC(__imp__Nome)` num arquivo
src/kernel/*_exports.cpp e rode este script de novo; o stub correspondente some.

Uso: python tools/gen_import_stubs.py [raiz-do-projeto]
"""

import re
import sys
from pathlib import Path

IMPORT_RE = re.compile(r"PPC_EXTERN_FUNC\((__imp__\w+)\)")
IMPL_RE = re.compile(r"^PPC_FUNC\((__imp__\w+)\)", re.MULTILINE)

HEADER = """\
// GERADO por tools/gen_import_stubs.py -- nao edite a mao.
// Stubs de log para os imports do kernel/XAM sem implementacao real em src/kernel/*_exports.cpp.
// Para implementar um import, defina PPC_FUNC(__imp__Nome) num *_exports.cpp e rode o script.

#include "recompiled/ppc_context.h"

#include <atomic>
#include <cstdio>

#define UNIMPLEMENTED_IMPORT(name)                                                         \\
    PPC_FUNC(__imp__##name) {                                                              \\
        static std::atomic<bool> logged{false};                                            \\
        if (!logged.exchange(true)) {                                                      \\
            std::fprintf(stderr, "[Kernel] import nao implementado: %s\\n", #name);         \\
        }                                                                                  \\
        ctx.r3.u64 = 0;                                                                    \\
    }

"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    shared = root / "src" / "recompiled" / "ppc_recomp_shared.h"
    kernel_dir = root / "src" / "kernel"
    out = kernel_dir / "import_stubs.cpp"

    imported = list(dict.fromkeys(IMPORT_RE.findall(shared.read_text(encoding="utf-8"))))
    if not imported:
        print(f"erro: nenhum __imp__ encontrado em {shared}", file=sys.stderr)
        return 1

    implemented = set()
    for path in sorted(kernel_dir.glob("*_exports.cpp")):
        implemented.update(IMPL_RE.findall(path.read_text(encoding="utf-8")))

    unknown = sorted(implemented - set(imported))
    if unknown:
        print("erro: PPC_FUNC definido para import que o jogo nao usa (typo?): " + ", ".join(unknown),
              file=sys.stderr)
        return 1

    stubs = [name for name in imported if name not in implemented]
    lines = [f"UNIMPLEMENTED_IMPORT({name[len('__imp__'):]})" for name in stubs]
    out.write_text(HEADER + "\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    print(f"{len(imported)} imports: {len(implemented)} implementados, {len(stubs)} stubs -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
