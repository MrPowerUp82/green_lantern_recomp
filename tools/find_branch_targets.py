#!/usr/bin/env python3
"""Declara em config.toml as entradas de funcao que o XenonRecomp nao detectou.

O gerador marca `// ERROR <addr>` quando um branch/tail call cai num endereco que nao e inicio de
funcao (cadeias de _savegpr/_restgpr, blocos compartilhados de epilogo). Cada um desses enderecos
vira uma entrada `{ address, size }` no bloco gerado de config.toml, com o tamanho ate o fim da
funcao que o contem (inicio da proxima funcao).

Uso: python tools/find_branch_targets.py [raiz-do-projeto]
Rode, regenere as unidades e repita ate nao restar `// ERROR`.
"""

import bisect
import re
import sys
from pathlib import Path

BEGIN = "# BEGIN generated-functions (tools/find_branch_targets.py)"
END = "# END generated-functions"

FN_RE = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", re.MULTILINE)
ERR_RE = re.compile(r"// ERROR ([0-9A-F]+)")
ENTRY_RE = re.compile(r"address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)")


SWITCH_RE = re.compile(r"base = 0x([0-9A-Fa-f]+)[^\[]*?labels = \[([^\]]*)\]", re.DOTALL)


def parse_switches(path: Path):
    if not path.exists():
        return []
    result = []
    for base, labels in SWITCH_RE.findall(path.read_text(encoding="utf-8")):
        result.append((int(base, 16), [int(x, 16) for x in labels.replace(" ", "").split(",") if x]))
    return result


SLOW_FUNCTION_LINES = 5000


def write_slow_units(recompiled: Path) -> None:
    """Unidades com funcoes gigantes: o otimizador do MSVC leva horas nelas, entao vao sem /O2."""
    slow = []
    for path in sorted(recompiled.glob("ppc_recomp.*.cpp"), key=lambda p: int(p.name.split(".")[1])):
        longest = 0
        start = None
        with path.open(encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle):
                if line.startswith("PPC_FUNC_IMPL("):
                    start = number
                elif line.startswith("}") and start is not None:
                    longest = max(longest, number - start)
                    start = None
        if longest >= SLOW_FUNCTION_LINES:
            slow.append(path.name)
    body = "# Gerado por tools/find_branch_targets.py: unidades com funcoes de milhares de linhas.\n"
    body += "set(PPC_SLOW_UNITS " + " ".join(slow) + ")\n"
    (recompiled / "slow_units.cmake").write_text(body, encoding="utf-8", newline="\n")
    print(f"{len(slow)} unidades com funcoes gigantes: {' '.join(slow)}")


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    config = root / "config.toml"
    recompiled = root / "src" / "recompiled"

    bases: set[int] = set()
    targets: set[int] = set()
    for path in recompiled.glob("ppc_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        bases.update(int(m, 16) for m in FN_RE.findall(text))
        targets.update(int(m, 16) for m in ERR_RE.findall(text))

    ordered = sorted(bases)
    text = config.read_text(encoding="utf-8")
    existing: dict[int, int] = {}
    if BEGIN in text:
        block = text[text.index(BEGIN):text.index(END)]
        existing = {int(a, 16): int(s, 16) for a, s in ENTRY_RE.findall(block)}
        text = text[:text.index(BEGIN)].rstrip("\n") + "\n" + text[text.index(END) + len(END):].lstrip("\n")

    added = 0
    for target in sorted(targets):
        if target in bases:
            continue
        idx = bisect.bisect_right(ordered, target)
        end = ordered[idx] if idx < len(ordered) else target + 4
        size = end - target
        if existing.get(target) != size:
            added += 1
        existing[target] = size

    # Funcoes cujo `switch` salta para alem do fim detectado: estende ate cobrir todos os rotulos.
    extended = 0
    all_bases = sorted(set(ordered) | set(existing))
    for bctr, labels in parse_switches(root / "tools" / "jump_table.toml"):
        # Todas as funcoes que contem o bctr: a natural (pdata) e as entradas explicitas que a sobrepoem.
        owner_index = bisect.bisect_right(all_bases, bctr) - 1
        if owner_index < 0:
            continue
        owners = {all_bases[owner_index]}
        owners.update(base for base, size in existing.items() if base <= bctr < base + size)
        for owner in owners:
            if owner in existing:
                owner_end = owner + existing[owner]
            else:
                pos = bisect.bisect_right(all_bases, owner)
                owner_end = all_bases[pos] if pos < len(all_bases) else owner + 4
            if bctr >= owner_end:
                continue
            needed_end = owner_end
            for label in labels:
                if label < owner or owner <= label < owner_end:
                    continue
                idx = bisect.bisect_right(all_bases, label)
                label_end = all_bases[idx] if idx < len(all_bases) else label + 4
                needed_end = max(needed_end, label_end)
            if needed_end > owner_end:
                existing[owner] = needed_end - owner
                extended += 1

    lines = [BEGIN, "functions = ["]
    for address in sorted(existing):
        lines.append(f"    {{ address = 0x{address:X}, size = 0x{existing[address]:X} }},")
    lines += ["]", END, ""]
    config.write_text(text.rstrip("\n") + "\n\n" + "\n".join(lines), encoding="utf-8", newline="\n")

    print(f"{len(targets)} destinos sem funcao, {extended} funcoes estendidas por switch, {len(existing)} entradas ({added} novas/alteradas) em {config}")
    write_slow_units(recompiled)
    return 0


if __name__ == "__main__":
    sys.exit(main())
