#!/usr/bin/env python3
"""
Utilitário de diagnóstico e inspeção de cabeçalhos de executáveis Xbox 360 (XEX2).
Analisa Entry Point, Base Address, Bibliotecas Importadas e Tabela de Ordinais.
"""

import sys
import os
import struct

def parse_xex(file_path):
    if not os.path.exists(file_path):
        print(f"Erro: Arquivo '{file_path}' não encontrado.")
        return

    with open(file_path, "rb") as f:
        magic = f.read(4)
        if magic != b"XEX2":
            print(f"Formato inválido. Magic esperado: XEX2, obtido: {magic}")
            return

        mod_flags, pe_offset, res, cert_offset, opt_hdr_count = struct.unpack(">IIIII", f.read(20))
        print("=" * 60)
        print("XEX2 Header Info")
        print("=" * 60)
        print(f"PE Image Offset:       0x{pe_offset:08X}")
        print(f"Certificate Offset:    0x{cert_offset:08X}")
        print(f"Optional Header Count: {opt_hdr_count}")
        print(f"Module Flags:          0x{mod_flags:08X}")

        headers = {}
        for i in range(opt_hdr_count):
            key, val = struct.unpack(">II", f.read(8))
            headers[key] = val

        # Extração de Chaves Críticas
        entry_point = headers.get(0x00010100)
        image_base = headers.get(0x00010201)
        import_libraries_offset = headers.get(0x000103FF)
        exec_info_offset = headers.get(0x000183FF)

        if entry_point is not None:
            print(f"Entry Point:           0x{entry_point:08X}")
        if image_base is not None:
            print(f"Image Base Address:    0x{image_base:08X}")

        if exec_info_offset is not None:
            f.seek(exec_info_offset)
            exec_data = f.read(24)
            media_id, version, base_version, title_id, platform, exec_table, disc_num, disc_count = struct.unpack(">IIIIBBBB", exec_data[:20])
            print(f"Title ID:              0x{title_id:08X}")
            print(f"Media ID:              0x{media_id:08X}")
            print(f"Game Version:          {version}")

        # Análise das bibliotecas importadas
        if import_libraries_offset is not None:
            print("\n" + "=" * 60)
            print("Import Libraries & Imports")
            print("=" * 60)
            f.seek(import_libraries_offset)
            size, count = struct.unpack(">II", f.read(8))
            str_count = struct.unpack(">I", f.read(4))[0]
            
            raw_str_data = f.read(size - 12)
            # Parse zero-terminated strings
            pos = 0
            libs = []
            for _ in range(str_count):
                end = raw_str_data.find(b"\x00", pos)
                if end != -1:
                    libs.append(raw_str_data[pos:end].decode("latin1"))
                    pos = end + 1

            print(f"Bibliotecas requeridas ({len(libs)}): {', '.join(libs)}")

if __name__ == "__main__":
    target = os.path.join(os.path.dirname(__file__), "..", "game", "default.xex")
    if len(sys.argv) > 1:
        target = sys.argv[1]
    parse_xex(target)
