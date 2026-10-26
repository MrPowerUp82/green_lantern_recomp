#!/usr/bin/env python3
"""
Extrai o microcodigo Xenos dos containers de shader do XDK (magic 0x102A110x) de um arquivo
binario (PE descompactado por unpack_xex.py ou um pak ja descompactado).

Cada shader vira <saida>/<offset>.<vs|ps>.bin com os dwords do microcodigo em big-endian,
exatamente como o jogo os entrega ao IM_LOAD. Os arquivos servem de corpus para o teste do
tradutor de shaders (gpu_shader_tests) e nao devem ir para o Git (sao dados do jogo).

Layout do container (big-endian), visto nos 192 shaders do default.xex:
  +0x00 flags: 0x102A1100 = pixel shader, 0x102A1101 = vertex shader
  +0x04 tamanho da parte virtual (cabecalho + tabelas); o microcodigo vem logo depois
  +0x08 tamanho da parte fisica
  +0x18 offset da struct do shader: +0x00 offset do microcodigo na parte fisica, +0x04 tamanho em bytes

Uso: python tools/extract_shaders.py <binario> <pasta de saida>
"""

import os
import struct
import sys

MAGIC = b"\x10\x2A\x11"


def extract(data, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    counts = {"vs": 0, "ps": 0}
    pos = data.find(MAGIC)
    while pos != -1:
        if pos % 4 == 0 and pos + 0x20 <= len(data) and data[pos + 3] in (0, 1):
            flags, virtual_size, physical_size = struct.unpack(">III", data[pos:pos + 12])
            shader_offset = struct.unpack(">I", data[pos + 0x18:pos + 0x1C])[0]
            if shader_offset + 8 <= virtual_size and pos + virtual_size + physical_size <= len(data):
                code_offset, code_size = struct.unpack(
                    ">II", data[pos + shader_offset:pos + shader_offset + 8])
                start = pos + virtual_size + code_offset
                if code_size and code_size % 12 == 0 and code_offset + code_size <= physical_size:
                    kind = "vs" if flags & 1 else "ps"
                    with open(os.path.join(out_dir, f"{pos:08x}.{kind}.bin"), "wb") as f:
                        f.write(data[start:start + code_size])
                    counts[kind] += 1
        pos = data.find(MAGIC, pos + 1)
    return counts


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    counts = extract(data, sys.argv[2])
    print(f"{counts['vs']} vertex shaders, {counts['ps']} pixel shaders em {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
