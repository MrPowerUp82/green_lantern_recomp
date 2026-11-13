#!/usr/bin/env python3
"""Extrai as tabelas de salto (switch) do jogo e grava tools/jump_table.toml para o XenonRecomp.

O XenonAnalyse so reconhece um formato de tabela; este jogo usa tambem tabelas de offsets de 16 bits
(`lhzx`) e absolutas. Para cada `bctr` o script percorre o codigo para tras, avaliando de forma
simbolica as instrucoes que montam o destino:

    destino = base + tabela[indice]           (tabela de bytes, halfwords ou words; com deslocamento opcional)
    destino = tabela[indice]                  (tabela absoluta de words)

O tamanho da tabela vem do `cmplwi indice, N` + `bgt/bge` que a protege.

Uso: python tools/find_jump_tables.py [imagem] [saida.toml]   (padrao: game/default.image, tools/jump_table.toml)
"""

import struct
import sys
from pathlib import Path

BASE = 0x82000000
CODE_BEGIN = 0x82140000
CODE_END = CODE_BEGIN + 0x6358EC
LOOKBACK = 40


def rd32(image, addr):
    off = addr - BASE
    return struct.unpack_from(">I", image, off)[0]


def rd16(image, addr):
    off = addr - BASE
    return struct.unpack_from(">H", image, off)[0]


def sx16(value):
    return value - 0x10000 if value & 0x8000 else value


class Insn:
    __slots__ = ("addr", "raw", "op")

    def __init__(self, addr, raw):
        self.addr = addr
        self.raw = raw
        self.op = raw >> 26

    def rt(self):
        return (self.raw >> 21) & 31

    ra = lambda self: (self.raw >> 16) & 31
    rb = lambda self: (self.raw >> 11) & 31
    simm = lambda self: sx16(self.raw & 0xFFFF)
    uimm = lambda self: self.raw & 0xFFFF
    xo = lambda self: (self.raw >> 1) & 0x3FF


def is_branch(i):
    # b, bc, bclr/bcctr (19 com xo 16/528), exceto bctr tratado a parte
    return i.op in (16, 18) or (i.op == 19 and i.xo() in (16, 528))


def resolve_const(image, insns, index, reg, depth=0):
    """Valor constante de `reg` imediatamente antes de insns[index], via lis/addi/ori/mr (ou None)."""
    if depth > 6:
        return None
    for k in range(index - 1, max(index - LOOKBACK, -1), -1):
        i = insns[k]
        if is_branch(i):
            return None
        raw = i.raw
        if i.op == 15 and i.rt() == reg:  # addis rt, ra, simm
            ra = i.ra()
            if ra == 0:
                return (i.simm() << 16) & 0xFFFFFFFF
            base = resolve_const(image, insns, k, ra, depth + 1)
            return None if base is None else (base + (i.simm() << 16)) & 0xFFFFFFFF
        if i.op == 14 and i.rt() == reg:  # addi rt, ra, simm
            ra = i.ra()
            if ra == 0:
                return i.simm() & 0xFFFFFFFF
            base = resolve_const(image, insns, k, ra, depth + 1)
            return None if base is None else (base + i.simm()) & 0xFFFFFFFF
        if i.op == 24 and i.ra() == reg:  # ori ra, rs, uimm
            base = resolve_const(image, insns, k, i.rt(), depth + 1)
            return None if base is None else (base | i.uimm()) & 0xFFFFFFFF
        if i.op == 31 and i.xo() == 444 and i.ra() == reg and i.rt() == i.rb():  # mr ra, rs
            return resolve_const(image, insns, k, i.rt(), depth + 1)
        if writes_reg(i, reg):
            return None
    return None


def writes_reg(i, reg):
    op = i.op
    raw = i.raw
    if raw == 0x60000000:  # nop (ori r0,r0,0)
        return False
    if op in (14, 15, 8, 12, 13, 7, 32, 34, 40, 42, 33, 35, 41, 43, 58, 48, 50, 56, 54):
        return i.rt() == reg and op not in (54, 56, 50, 48)  # lfs/stfs etc. nao escrevem GPR
    if op in (24, 25, 26, 27, 28, 29, 21, 20, 22, 23):  # logicas e rotacoes: rA e o destino
        return i.ra() == reg
    if op == 31:
        xo = i.xo()
        if xo in (266, 10, 8, 136, 138, 200, 202, 235, 233, 40, 104, 234, 232, 459, 491, 75, 11, 9,
                  23, 87, 279, 343, 375, 311, 55, 119, 21, 20, 53, 54, 341, 373, 407, 439, 790, 534):
            return i.rt() == reg
        if xo in (28, 60, 124, 284, 316, 412, 444, 476, 536, 824, 792, 24, 26, 922, 954, 986, 794, 826):
            return i.ra() == reg
    return False


def find_table(image, insns, bctr_index):
    """Retorna (idx_reg, labels, tabela_addr) ou None."""
    # mtctr rS imediatamente antes (permitindo instrucoes nao relacionadas entre eles)
    mtctr_k = None
    for k in range(bctr_index - 1, max(bctr_index - 8, -1), -1):
        i = insns[k]
        if i.op == 31 and i.xo() == 467 and ((i.raw >> 11) & 0x3FF) == 0x120:  # mtspr CTR
            mtctr_k = k
            break
        if is_branch(i):
            return None
    if mtctr_k is None:
        return None
    src = insns[mtctr_k].rt()

    # Definicao de src
    for k in range(mtctr_k - 1, max(mtctr_k - LOOKBACK, -1), -1):
        i = insns[k]
        if is_branch(i):
            return None
        if i.op == 31 and i.xo() == 266 and i.rt() == src:  # add src, a, b
            return decode_add(image, insns, k, i.ra(), i.rb())
        if i.op == 31 and i.xo() == 23 and i.rt() == src:  # lwzx src, a, b  (absoluta)
            return decode_absolute(image, insns, k)
        if writes_reg(i, src):
            return None
    return None


def decode_absolute(image, insns, k):
    i = insns[k]
    table = resolve_const(image, insns, k, i.ra())
    idx = shifted_index(insns, k, i.rb(), 2)
    if table is None or idx is None:
        return None
    return idx, table, 4, 0, None


def shifted_index(insns, k, reg, expect_shift):
    """Registrador de indice cujo valor << expect_shift esta em `reg` (rlwinm/slwi/mulli), ou None."""
    for j in range(k - 1, max(k - LOOKBACK, -1), -1):
        i = insns[j]
        if is_branch(i):
            return None
        if i.op == 21 and i.ra() == reg:  # rlwinm ra, rs, sh, mb, me
            sh = (i.raw >> 11) & 31
            mb = (i.raw >> 6) & 31
            me = (i.raw >> 1) & 31
            if sh == expect_shift and mb == 0 and me == 31 - expect_shift:
                return i.rt()
            return None
        if expect_shift == 0 and writes_reg(i, reg):
            return reg if False else None
        if writes_reg(i, reg):
            return None
    return None


def decode_add(image, insns, k, ra, rb):
    """add src, ra, rb: um lado e a base constante, o outro o valor carregado da tabela."""
    for base_reg, load_reg in ((ra, rb), (rb, ra)):
        base2 = resolve_const(image, insns, k, base_reg)
        if base2 is None:
            continue
        # carregamento (opcionalmente deslocado) de load_reg
        extra_shift = 0
        want = load_reg
        for j in range(k - 1, max(k - LOOKBACK, -1), -1):
            i = insns[j]
            if is_branch(i):
                break
            if i.op == 21 and i.ra() == want:  # rlwinm want, x, sh, 0, 31-sh  (valor << sh)
                sh = (i.raw >> 11) & 31
                mb = (i.raw >> 6) & 31
                me = (i.raw >> 1) & 31
                if mb == 0 and me == 31 - sh:
                    extra_shift = sh
                    want = i.rt()
                    continue
                break
            if i.op == 31 and i.rt() == want and i.xo() in (87, 279, 23):  # lbzx, lhzx, lwzx
                size = {87: 1, 279: 2, 23: 4}[i.xo()]
                table = resolve_const(image, insns, j, i.ra())
                if table is None:
                    table = resolve_const(image, insns, j, i.rb())
                    index_reg_shifted = i.ra()
                else:
                    index_reg_shifted = i.rb()
                if table is None:
                    break
                shift_log = {1: 0, 2: 1, 4: 2}[size]
                idx = shifted_index(insns, j, index_reg_shifted, shift_log) if shift_log else index_reg_shifted
                if idx is None:
                    break
                return idx, table, size, extra_shift, base2
            if writes_reg(i, want):
                break
    return None


def table_size(insns, bctr_index, idx_reg):
    """N + 1 do `cmplwi idx, N` que protege o switch (procurando para tras). Segue copias `mr`."""
    lo = max(bctr_index - 80, 0)
    aliases = {idx_reg}
    for _ in range(3):  # propaga aliases em ambos os sentidos algumas vezes
        for k in range(lo, bctr_index):
            i = insns[k]
            if i.op == 31 and i.xo() == 444 and i.rt() == i.rb():  # mr ra, rs
                dest, src = i.ra(), i.rt()
                if dest in aliases or src in aliases:
                    aliases.add(dest)
                    aliases.add(src)
    for k in range(bctr_index - 1, lo - 1, -1):
        i = insns[k]
        if i.op in (10, 11) and i.ra() in aliases:  # cmpli / cmpi
            return (i.raw & 0xFFFF) + 1
    return None


def main():
    root = Path(__file__).resolve().parent.parent
    image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "game" / "default.image"
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "tools" / "jump_table.toml"
    image = image_path.read_bytes()

    insns = []
    for addr in range(CODE_BEGIN, CODE_END, 4):
        insns.append(Insn(addr, rd32(image, addr)))

    found = []
    unmatched = []
    for index, insn in enumerate(insns):
        if insn.raw != 0x4E800420:  # bctr
            continue
        lo = max(index - 120, 0)
        window = insns[lo:index + 1]
        local_index = index - lo
        result = find_table(image, window, local_index)
        if not result:
            unmatched.append(insn.addr)
            continue
        idx_reg, table, size, extra_shift, base2 = result
        count = table_size(window, local_index, idx_reg)
        guessed = False
        if not count:
            # Sem comparacao na janela (o limite foi verificado por outro caminho): le entradas enquanto
            # forem enderecos de codigo proximos do bctr.
            count = 64
            guessed = True
        if count > 2048:
            unmatched.append(insn.addr)
            continue

        labels = []
        ok = True
        for n in range(count):
            if size == 1:
                entry = image[table - BASE + n]
            elif size == 2:
                entry = rd16(image, table + 2 * n)
            else:
                entry = rd32(image, table + 4 * n)
            target = (entry << extra_shift) & 0xFFFFFFFF
            if base2 is not None:
                target = (base2 + target) & 0xFFFFFFFF
            valid = CODE_BEGIN <= target < CODE_END and not target & 3
            if guessed:
                valid = valid and abs(target - insn.addr) < 0x4000
                if not valid:
                    break  # fim da tabela
            elif not valid:
                ok = False
                break
            labels.append(target)
        if not ok:
            unmatched.append(insn.addr)
            continue
        if guessed and len(labels) < 2:
            unmatched.append(insn.addr)
            continue
        found.append((insn.addr, idx_reg, labels))

    lines = ["# Gerado por tools/find_jump_tables.py", ""]
    for addr, idx_reg, labels in found:
        lines.append("[[switch]]")
        lines.append(f"base = 0x{addr:X}")
        lines.append(f"r = {idx_reg}")
        lines.append("labels = [" + ", ".join(f"0x{label:X}" for label in labels) + "]")
        lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")

    total = len(found) + len(unmatched)
    print(f"{total} bctr: {len(found)} tabelas extraidas, {len(unmatched)} sem tabela (tail calls ou nao reconhecidos)")
    if unmatched and "--list" in sys.argv:
        print(" ".join(f"{a:X}" for a in unmatched))


if __name__ == "__main__":
    main()
