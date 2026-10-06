#pragma once

#include <cstdint>

// Formato dos pacotes PM4 do command processor do Xenos.
namespace Gpu::Pm4 {
    // 'SWAP' (0x53574150): assinatura do pacote XE_SWAP que VdSwap escreve no ring.
    constexpr uint32_t kSwapSignature = 0x53574150;

    enum Opcode : uint32_t {
        kNop = 0x10,
        kRegRmw = 0x21,
        kDrawIndx = 0x22,
        kSetConstant = 0x2D,
        kLoadAluConstant = 0x2F,
        kDrawIndx2 = 0x36,
        kIndirectBufferPfd = 0x37,
        kMemWrite = 0x3D,
        kRegToMem = 0x3E,
        kWaitRegMem = 0x3C,
        kIndirectBuffer = 0x3F,
        kCondWrite = 0x45,
        kEventWrite = 0x46,
        kMeInit = 0x48,
        kInterrupt = 0x54,
        kSetConstant2 = 0x55,
        kSetShaderConstants = 0x56,
        kEventWriteShd = 0x58,
        kEventWriteExt = 0x5A,
        kXeSwap = 0x64,
    };

    // Tipo 0: ttcccccc cccccccc oiiiiiii iiiiiiii (o = escreve sempre o mesmo registrador)
    constexpr uint32_t MakeType0(uint32_t baseIndex, uint32_t count, bool oneReg = false) {
        return (((count - 1) & 0x3FFFu) << 16) | (oneReg ? 0x8000u : 0u) | (baseIndex & 0x7FFFu);
    }

    // Tipo 2: filler sem payload.
    constexpr uint32_t MakeType2() { return 2u << 30; }

    // Tipo 3: ttcccccc cccccccc ?ooooooo ???????p
    constexpr uint32_t MakeType3(uint32_t opcode, uint32_t count, bool predicate = false) {
        return (3u << 30) | (((count - 1) & 0x3FFFu) << 16) | ((opcode & 0x7Fu) << 8) | (predicate ? 1u : 0u);
    }

    struct Header {
        uint32_t type = 0;       // 0..3
        uint32_t count = 0;      // dwords de payload (tipos 0 e 3); 0 nos demais
        uint32_t baseIndex = 0;  // tipo 0: primeiro registrador
        bool oneReg = false;     // tipo 0: todos os dwords vao para baseIndex
        uint32_t opcode = 0;     // tipo 3
        bool predicate = false;  // tipo 3 (decodificado, ignorado no M1)
    };

    constexpr Header DecodeHeader(uint32_t raw) {
        Header h;
        h.type = raw >> 30;
        if (h.type == 0) {
            h.count = ((raw >> 16) & 0x3FFFu) + 1;
            h.baseIndex = raw & 0x7FFFu;
            h.oneReg = ((raw >> 15) & 1u) != 0;
        } else if (h.type == 3) {
            h.count = ((raw >> 16) & 0x3FFFu) + 1;
            h.opcode = (raw >> 8) & 0x7Fu;
            h.predicate = (raw & 1u) != 0;
        }
        return h;
    }
}
