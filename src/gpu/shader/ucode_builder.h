#pragma once

#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

#include <cstdint>
#include <vector>

// Montador minimo de microcodigo Xenos, usado pelos testes do tradutor e pela cena do
// gpu_selftest. Segue os campos de bits de xenia/gpu/ucode.h.
namespace Gpu::Shader::Ucode {
    namespace ucode = xe::gpu::ucode;
    namespace xenos = xe::gpu::xenos;

    // Instrucao de controle de fluxo (48 bits: dword_0 + 16 bits baixos de dword_1).
    struct Cf {
        uint32_t lo = 0;
        uint32_t hi = 0;  // 16 bits
    };

    // exec/exec_end: `count` instrucoes a partir de `address` (em instrucoes de 3 dwords).
    // `fetchMask`: bit k = a k-esima instrucao e fetch (senao ALU).
    inline Cf Exec(uint32_t address, uint32_t count, uint32_t fetchMask, bool end) {
        uint32_t sequence = 0;
        for (uint32_t k = 0; k < count; ++k) {
            if (fetchMask & (1u << k)) sequence |= 1u << (k * 2);
        }
        Cf cf;
        cf.lo = (address & 0xFFF) | ((count & 7) << 12) | ((sequence & 0xFFF) << 16);
        const ucode::ControlFlowOpcode op = end ? ucode::ControlFlowOpcode::kExecEnd : ucode::ControlFlowOpcode::kExec;
        cf.hi = (1u << 11) /* absoluto */ | (uint32_t(op) << 12);
        return cf;
    }

    // cond_exec com bool constant (condition: executa se o bool == condition).
    inline Cf CondExec(uint32_t address, uint32_t count, uint32_t fetchMask, uint32_t boolAddress, bool condition,
                       bool end) {
        Cf cf = Exec(address, count, fetchMask, false);
        const ucode::ControlFlowOpcode op =
            end ? ucode::ControlFlowOpcode::kCondExecEnd : ucode::ControlFlowOpcode::kCondExec;
        cf.hi = ((boolAddress & 0xFF) << 2) | ((condition ? 1u : 0u) << 10) | (1u << 11) | (uint32_t(op) << 12);
        return cf;
    }

    inline Cf LoopStart(uint32_t skipAddress, uint32_t loopId) {
        Cf cf;
        cf.lo = (skipAddress & 0x1FFF) | ((loopId & 0x1F) << 16);
        cf.hi = (1u << 11) | (uint32_t(ucode::ControlFlowOpcode::kLoopStart) << 12);
        return cf;
    }

    inline Cf LoopEnd(uint32_t bodyAddress, uint32_t loopId) {
        Cf cf;
        cf.lo = (bodyAddress & 0x1FFF) | ((loopId & 0x1F) << 16);
        cf.hi = (1u << 11) | (uint32_t(ucode::ControlFlowOpcode::kLoopEnd) << 12);
        return cf;
    }

    inline Cf Jump(uint32_t target, bool predicated = false, bool condition = true) {
        Cf cf;
        cf.lo = (target & 0x1FFF) | (predicated ? 0u : (1u << 13)) | (predicated ? (1u << 14) : 0u);
        cf.hi = ((condition ? 1u : 0u) << 10) | (1u << 11) | (uint32_t(ucode::ControlFlowOpcode::kCondJmp) << 12);
        return cf;
    }

    inline Cf Nop() { return Cf{}; }

    // Operando de ALU: registrador temporario ou constante, swizzle absoluto (ex.: "xyzw").
    struct Src {
        uint32_t reg = 0;
        bool temp = true;
        const char* swizzle = "xyzw";
        bool negate = false;
    };
    inline Src R(uint32_t reg, const char* swizzle = "xyzw", bool negate = false) { return { reg, true, swizzle, negate }; }
    inline Src C(uint32_t reg, const char* swizzle = "xyzw", bool negate = false) { return { reg, false, swizzle, negate }; }

    // Swizzle de ALU: 2 bits por componente, relativo ao indice do componente.
    inline uint32_t AluSwizzle(const char* s) {
        uint32_t result = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            const char ch = s[c] ? s[c] : s[0];
            const uint32_t index = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : 3;
            result |= ((index - c) & 3) << (c * 2);
        }
        return result;
    }

    struct Alu {
        ucode::AluVectorOpcode vectorOp = ucode::AluVectorOpcode::kMax;
        ucode::AluScalarOpcode scalarOp = ucode::AluScalarOpcode::kRetainPrev;
        uint32_t vectorDest = 0;
        uint32_t vectorMask = 0;
        uint32_t scalarDest = 0;
        uint32_t scalarMask = 0;
        bool exportData = false;  // vectorDest e o registrador de export (62 = posicao, 0..15 = interpoladores/cores)
        bool vectorClamp = false;
        Src src[3];
    };

    inline void EmitAlu(std::vector<uint32_t>& out, const Alu& a) {
        uint32_t d0 = (a.vectorDest & 0x3F) | ((a.scalarDest & 0x3F) << 8) | ((a.exportData ? 1u : 0u) << 15) |
                      ((a.vectorMask & 0xF) << 16) | ((a.scalarMask & 0xF) << 20) | ((a.vectorClamp ? 1u : 0u) << 24) |
                      ((uint32_t(a.scalarOp) & 0x3F) << 26);
        uint32_t d1 = (AluSwizzle(a.src[2].swizzle) & 0xFF) | ((AluSwizzle(a.src[1].swizzle) & 0xFF) << 8) |
                      ((AluSwizzle(a.src[0].swizzle) & 0xFF) << 16) | ((a.src[2].negate ? 1u : 0u) << 24) |
                      ((a.src[1].negate ? 1u : 0u) << 25) | ((a.src[0].negate ? 1u : 0u) << 26);
        uint32_t d2 = (a.src[2].reg & 0xFF) | ((a.src[1].reg & 0xFF) << 8) | ((a.src[0].reg & 0xFF) << 16) |
                      ((uint32_t(a.vectorOp) & 0x1F) << 24) | ((a.src[2].temp ? 1u : 0u) << 29) |
                      ((a.src[1].temp ? 1u : 0u) << 30) | ((a.src[0].temp ? 1u : 0u) << 31);
        out.push_back(d0);
        out.push_back(d1);
        out.push_back(d2);
    }

    // mov via max(src, src) (como o D3D faz).
    inline Alu Mov(uint32_t dest, Src src, uint32_t mask = 0xF, bool exportData = false) {
        Alu a;
        a.vectorOp = ucode::AluVectorOpcode::kMax;
        a.vectorDest = dest;
        a.vectorMask = mask;
        a.exportData = exportData;
        a.src[0] = src;
        a.src[1] = src;
        return a;
    }

    inline Alu Vector(ucode::AluVectorOpcode op, uint32_t dest, uint32_t mask, Src s0, Src s1 = {}, Src s2 = {},
                      bool exportData = false) {
        Alu a;
        a.vectorOp = op;
        a.vectorDest = dest;
        a.vectorMask = mask;
        a.exportData = exportData;
        a.src[0] = s0;
        a.src[1] = s1;
        a.src[2] = s2;
        return a;
    }

    // Destino de fetch: swizzle de 3 bits por componente (0..3 = xyzw, 4 = 0, 5 = 1, 7 = manter).
    inline uint32_t FetchDestSwizzle(const char* s) {
        uint32_t result = 0;
        for (uint32_t c = 0; c < 4; ++c) {
            const char ch = s[c];
            const uint32_t v = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : ch == '0' ? 4 : ch == '1' ? 5 : 7;
            result |= v << (c * 3);
        }
        return result;
    }

    // vfetch completo: indice em r[src].x, fetch constant `fetchIndex` (0..95), formato e offset em dwords.
    inline void EmitVertexFetch(std::vector<uint32_t>& out, uint32_t dest, const char* destSwizzle, uint32_t src,
                                uint32_t fetchIndex, xenos::VertexFormat format, uint32_t strideDwords,
                                int32_t offsetDwords, bool isFloat) {
        const uint32_t constIndex = fetchIndex / 3;
        const uint32_t constSel = fetchIndex % 3;
        uint32_t d0 = (uint32_t(ucode::FetchOpcode::kVertexFetch) & 0x1F) | ((src & 0x3F) << 5) |
                      ((dest & 0x3F) << 12) | (1u << 19) | ((constIndex & 0x1F) << 20) | ((constSel & 3) << 25);
        // num_format_all = 0: inteiros normalizados (formatos float ignoram); sem sinal.
        (void)isFloat;
        uint32_t d1 = (FetchDestSwizzle(destSwizzle) & 0xFFF) | ((uint32_t(format) & 0x3F) << 16);
        uint32_t d2 = (strideDwords & 0xFF) | ((uint32_t(offsetDwords) & 0x7FFFFF) << 8);
        out.push_back(d0);
        out.push_back(d1);
        out.push_back(d2);
    }

    // tfetch 2D: coordenadas r[src].xy, fetch constant 0..31, filtros da fetch constant.
    inline void EmitTextureFetch2D(std::vector<uint32_t>& out, uint32_t dest, const char* destSwizzle, uint32_t src,
                                   uint32_t fetchConstant) {
        uint32_t d0 = (uint32_t(ucode::FetchOpcode::kTextureFetch) & 0x1F) | ((src & 0x3F) << 5) |
                      ((dest & 0x3F) << 12) | ((fetchConstant & 0x1F) << 20) | ((0u | (1u << 2)) << 26);  // src .xy
        const uint32_t useFetchConst = 3;  // kUseFetchConst nos filtros
        uint32_t d1 = (FetchDestSwizzle(destSwizzle) & 0xFFF) | (useFetchConst << 12) | (useFetchConst << 14) |
                      (useFetchConst << 16) | (7u << 18) /* aniso: usa a fetch constant */ |
                      (useFetchConst << 24) | (useFetchConst << 26) | (1u << 28) /* LOD calculado */;
        uint32_t d2 = (uint32_t(xenos::FetchOpDimension::k2D) << 14);
        out.push_back(d0);
        out.push_back(d1);
        out.push_back(d2);
    }

    // Monta o programa: instrucoes de controle de fluxo (pares em 3 dwords) e depois as de exec.
    // As exec referenciam enderecos absolutos; use CfPairCount() para saber onde as instrucoes comecam.
    inline uint32_t CfPairCount(size_t cfCount) { return uint32_t((cfCount + 1) / 2); }

    inline std::vector<uint32_t> Assemble(const std::vector<Cf>& cfs, const std::vector<uint32_t>& instructions) {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < cfs.size(); i += 2) {
            const Cf a = cfs[i];
            const Cf b = i + 1 < cfs.size() ? cfs[i + 1] : Cf{};
            out.push_back(a.lo);
            out.push_back((a.hi & 0xFFFF) | (b.lo << 16));
            out.push_back((b.lo >> 16) | (b.hi << 16));
        }
        out.insert(out.end(), instructions.begin(), instructions.end());
        return out;
    }
}
