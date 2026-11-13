#!/usr/bin/env python3
"""Traduz RVAs do relatorio de falhas ("[Crash] #NN rip=... rva=0x...") em simbolos do mapa do linker.

Uso: python tools/map_lookup.py <log> [GreenLanternRecomp.map]
Sem log, le o relatorio de stdin. Mostra, para cada quadro, a funcao do mapa e o offset dentro dela
(`__imp__sub_XXXXXXXX` e o corpo da funcao do guest 0xXXXXXXXX).
"""

import bisect
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
map_path = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build" / "bin" / "GreenLanternRecomp.map"
log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace") if len(sys.argv) > 1 else sys.stdin.read()

text = map_path.read_text(encoding="utf-8", errors="replace")
preferred = re.search(r"Preferred load address is ([0-9a-fA-F]+)", text)
image_base = int(preferred.group(1), 16) if preferred else 0x140000000

# linhas: " 0001:00012340       name       0000000140013340 f   obj.obj"
symbols = []
for match in re.finditer(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f", text, re.MULTILINE):
    symbols.append((int(match.group(2), 16) - image_base, match.group(1)))
symbols.sort()
addresses = [s[0] for s in symbols]

for line in log.splitlines():
    m = re.search(r"#(\d+) rip=\S+ rva=0x([0-9A-Fa-f]+)", line)
    if not m:
        continue
    rva = int(m.group(2), 16)
    i = bisect.bisect_right(addresses, rva) - 1
    if i >= 0:
        name = symbols[i][1]
        guest = re.search(r"sub_([0-9A-F]{8})", name)
        print(f"#{m.group(1)}  {name}+0x{rva - symbols[i][0]:X}" + (f"   (guest 0x{guest.group(1)})" if guest else ""))
    else:
        print(f"#{m.group(1)}  ?")
