#!/usr/bin/env python3
"""
Descompressor e Descriptografador de XEX2 para o formato base descompactado (PE executável não criptografado).
Implementa o algoritmo padrão do XEX2 (AES-128-CBC com chave de varejo + Basic / Normal Decompression).
"""

import os
import sys
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend

# Chave de varejo oficial do Xbox 360 XEX2
XEX2_RETAIL_KEY = bytes([
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91
])

def unpack_xex(input_xex_path, output_unpacked_path):
    print(f"Lendo: {input_xex_path}")
    with open(input_xex_path, "rb") as f:
        data = f.read()

    magic = data[:4]
    if magic != b"XEX2":
        raise ValueError(f"Arquivo não é um XEX2 válido (magic={magic})")

    mod_flags, pe_offset, res, cert_offset, opt_hdr_count = struct.unpack(">IIIII", data[4:24])
    print(f"Header: PE Offset=0x{pe_offset:X}, Cert Offset=0x{cert_offset:X}, Opt Header Count={opt_hdr_count}")

    # Ler Headers Opcionais
    opt_headers = {}
    pos = 24
    for _ in range(opt_hdr_count):
        k, v = struct.unpack(">II", data[pos:pos+8])
        opt_headers[k] = v
        pos += 8

    # Security Info
    sec_info_raw = data[cert_offset:]
    sec_hdr_size, sec_img_size = struct.unpack(">II", sec_info_raw[:8])
    aes_key_enc = sec_info_raw[0x100 + 4 + 4 + 4 + 0x14 + 4 + 0x14 + 0x10 : 0x100 + 4 + 4 + 4 + 0x14 + 4 + 0x14 + 0x10 + 16]
    # Na estrutura do Xex2SecurityInfo:
    # 0x00: headerSize (4)
    # 0x04: imageSize (4)
    # 0x08: rsaSignature (256 = 0x100)
    # 0x108: unknown (4)
    # 0x10C: imageFlags (4)
    # 0x110: loadAddress (4)
    # 0x114: sectionDigest (20 = 0x14)
    # 0x128: importTableCount (4)
    # 0x12C: importTableDigest (20 = 0x14)
    # 0x140: xgd2MediaId (16 = 0x10)
    # 0x150: aesKey (16 = 0x10)
    aes_key_enc = sec_info_raw[0x150:0x160]
    print(f"Tamanho da Imagem Descompactada: {sec_img_size} bytes (0x{sec_img_size:X})")
    print(f"Chave AES Criptografada: {aes_key_enc.hex()}")

    # Decriptar a chave da imagem usando a XEX2_RETAIL_KEY
    backend = default_backend()
    zero_iv = b"\x00" * 16
    cipher = Cipher(algorithms.AES(XEX2_RETAIL_KEY), modes.CBC(zero_iv), backend=backend)
    decryptor = cipher.decryptor()
    session_key = decryptor.update(aes_key_enc) + decryptor.finalize()
    print(f"Chave de Sessão AES Decriptada: {session_key.hex()}")

    # Obter FileFormatInfo
    file_format_info_offset = opt_headers.get(0x000003FF)
    if not file_format_info_offset:
        raise ValueError("Header 0x000003FF (FileFormatInfo) não encontrado!")

    info_size, enc_type, comp_type = struct.unpack(">IHH", data[file_format_info_offset:file_format_info_offset+8])
    print(f"Formato: infoSize={info_size}, encType={enc_type}, compType={comp_type}")

    payload = data[pe_offset:]
    if enc_type == 1: # XEX_ENCRYPTION_NORMAL
        print(f"Decriptando payload de tamanho {len(payload)} bytes...")
        cipher = Cipher(algorithms.AES(session_key), modes.CBC(zero_iv), backend=backend)
        decryptor = cipher.decryptor()
        payload = decryptor.update(payload) + decryptor.finalize()

    # Descompressão
    uncompressed = bytearray()
    if comp_type == 0: # XEX_COMPRESSION_NONE
        print("Arquivo sem compressão adicional.")
        uncompressed = bytearray(payload[:sec_img_size])
    elif comp_type == 1: # XEX_COMPRESSION_BASIC
        num_blocks = (info_size - 8) // 8
        print(f"Descompactando Basic Compression ({num_blocks} blocos)...")
        block_pos = file_format_info_offset + 8
        src_pos = 0
        for b in range(num_blocks):
            dsize, zsize = struct.unpack(">II", data[block_pos:block_pos+8])
            block_pos += 8
            if dsize > 0:
                uncompressed.extend(payload[src_pos:src_pos+dsize])
                src_pos += dsize
            if zsize > 0:
                uncompressed.extend(b"\x00" * zsize)
        print(f"Descompactação básica concluída: {len(uncompressed)} bytes gerados.")
    else:
        raise NotImplementedError(f"Tipo de compressão {comp_type} requer descompressor LZX!")

    # Gravar imagem descompactada
    with open(output_unpacked_path, "wb") as f_out:
        f_out.write(uncompressed)

    print(f"Imagem PE base gerada com sucesso em: {output_unpacked_path} ({len(uncompressed)} bytes)")

if __name__ == "__main__":
    in_xex = sys.argv[1] if len(sys.argv) > 1 else r"game\default.xex"
    out_pe = sys.argv[2] if len(sys.argv) > 2 else r"game\default_unpacked.pe"
    unpack_xex(in_xex, out_pe)
