#!/usr/bin/env python3
"""
Scanner para identificação dos endereços virtuais das funções de convenção de chamada PPC ABI
utilizadas pelo compilador MSVC do Xbox 360:
- __restgprlr_14
- __savegprlr_14
- __restfpr_14
- __savefpr_14
- __restvmx_14
- __savevmx_14
- __restvmx_64
- __savevmx_64
- setjmp / longjmp
"""

import struct
import os

IMAGE_BASE = 0x82000000

def scan_helpers(pe_path):
    with open(pe_path, "rb") as f:
        data = f.read()

    pe_off = int.from_bytes(data[0x3C:0x40], "little")
    num_sections = struct.unpack("<H", data[pe_off+6:pe_off+8])[0]
    opt_hdr_size = struct.unpack("<H", data[pe_off+20:pe_off+22])[0]
    sec_start = pe_off + 24 + opt_hdr_size

    text_data = None
    text_vaddr = 0
    for i in range(num_sections):
        sec = data[sec_start + i*40 : sec_start + (i+1)*40]
        name = sec[:8].rstrip(b"\x00").decode("latin1")
        vsize, vaddr, raw_size, raw_ptr = struct.unpack("<IIII", sec[8:24])
        if name == ".text":
            text_data = data[raw_ptr:raw_ptr+raw_size]
            text_vaddr = vaddr
            break

    if not text_data:
        print("Seção .text não encontrada!")
        return {}

    print(f"Seção .text mapeada: VAddr=0x{text_vaddr:08X} (Tamanho: {len(text_data)} bytes)")
    results = {}

    # Instruções PPC em Big-Endian (32-bit words):
    # __savegprlr_14 salva r14..r31 e termina com blr (0x4E800020)
    # std r14, -144(r1) -> 0xF9C1FF70
    # std r15, -136(r1) -> 0xF9E1FF78
    # ...
    # mflr r0 -> 0x7C0802A6
    # blr -> 0x4E800020
    
    # Busca por padrões característicos de savegprlr
    # No PPC 360, savegprlr_14 geralmente começa salvando r14 e encadeia até r31
    for i in range(0, len(text_data) - 76, 4):
        word0 = int.from_bytes(text_data[i:i+4], "big")
        # std r14, -144(r1) = 0xF9C1FF70 ou similar
        if (word0 & 0xFFFF0000) == 0xF9C10000:
            # Verificar se os próximos são r15, r16, ...
            word1 = int.from_bytes(text_data[i+4:i+8], "big")
            if (word1 & 0xFFFF0000) == 0xF9E10000:
                addr = IMAGE_BASE + text_vaddr + i
                results["savegprlr_14"] = addr
                break

    # __restgprlr_14: ld r14, ... seguido por blr
    for i in range(0, len(text_data) - 76, 4):
        word0 = int.from_bytes(text_data[i:i+4], "big")
        # ld r14, -144(r1) = 0xE9C1FF70
        if (word0 & 0xFFFF0000) == 0xE9C10000:
            word1 = int.from_bytes(text_data[i+4:i+8], "big")
            if (word1 & 0xFFFF0000) == 0xE9E10000:
                addr = IMAGE_BASE + text_vaddr + i
                results["restgprlr_14"] = addr
                break

    # __savefpr_14: stfd fr14, ...
    for i in range(0, len(text_data) - 76, 4):
        word0 = int.from_bytes(text_data[i:i+4], "big")
        # stfd fr14 = 0xD9C1...
        if (word0 & 0xFFFF0000) == 0xD9C10000:
            word1 = int.from_bytes(text_data[i+4:i+8], "big")
            if (word1 & 0xFFFF0000) == 0xD9E10000:
                addr = IMAGE_BASE + text_vaddr + i
                results["savefpr_14"] = addr
                break

    # __restfpr_14: lfd fr14, ...
    for i in range(0, len(text_data) - 76, 4):
        word0 = int.from_bytes(text_data[i:i+4], "big")
        # lfd fr14 = 0xC9C1...
        if (word0 & 0xFFFF0000) == 0xC9C10000:
            word1 = int.from_bytes(text_data[i+4:i+8], "big")
            if (word1 & 0xFFFF0000) == 0xC9E10000:
                addr = IMAGE_BASE + text_vaddr + i
                results["restfpr_14"] = addr
                break

    print("Endereços dos Helpers ABI identificados:")
    for k, v in results.items():
        print(f"  {k:18s} = 0x{v:08X}")

    return results

if __name__ == "__main__":
    scan_helpers(r"game\default_unpacked.pe")
