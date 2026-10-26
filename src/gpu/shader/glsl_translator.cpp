#include "gpu/shader/glsl_translator.h"

#include "gpu/shader/draw_constants.h"

#include "xenia/gpu/ucode.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

namespace Gpu::Shader {
    namespace ucode = xe::gpu::ucode;
    using ucode::AluInstruction;
    using ucode::AluScalarOpcode;
    using ucode::AluVectorOpcode;
    using ucode::ControlFlowInstruction;
    using ucode::ControlFlowOpcode;
    using ucode::ExportRegister;
    using ucode::FetchOpcode;

    namespace {
        constexpr int kRegisterCount = 64;
        constexpr const char* kComponents = "xyzw";

        std::string Format(const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            va_list copy;
            va_copy(copy, args);
            const int size = std::vsnprintf(nullptr, 0, fmt, copy);
            va_end(copy);
            std::string result(size > 0 ? size_t(size) : 0, '\0');
            if (size > 0) std::vsnprintf(result.data(), result.size() + 1, fmt, args);
            va_end(args);
            return result;
        }

        // Funcoes auxiliares comuns aos dois estagios. As semanticas seguem o interpretador do Xenia
        // (Direct3D 9: 0 * qualquer coisa = 0; max/min escolhem o primeiro operando em empate).
        const char* kCommonPrelude = R"(
layout(std430, set = 0, binding = 0) readonly buffer XeRingU { uint xe_ring[]; };
layout(std430, set = 0, binding = 0) readonly buffer XeRingV { uvec4 xe_ring4[]; };
layout(push_constant) uniform XePush { uint cb; } xe_push;

const float XE_FLT_MAX = 3.402823466e+38;

uint xe_u(uint i) { return xe_ring[xe_push.cb + i]; }
float xe_sysf(uint i) { return uintBitsToFloat(xe_u(XE_SYS + i)); }
uint xe_sys(uint i) { return xe_u(XE_SYS + i); }
vec4 xe_c(int i) { return uintBitsToFloat(xe_ring4[(xe_push.cb >> 2u) + uint(clamp(i, 0, 511))]); }
bool xe_bool(uint i) { return ((xe_u(XE_BOOL + (i >> 5u)) >> (i & 31u)) & 1u) != 0u; }
uint xe_loop(uint i) { return xe_u(XE_LOOP + i); }
uint xe_fetch(uint i) { return xe_u(XE_FETCH + i); }

float xe_mul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xe_mul4(vec4 a, vec4 b) {
    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(a * b, vec4(0.0), z);
}
float xe_max(float a, float b) { return a >= b ? a : b; }
float xe_min(float a, float b) { return a < b ? a : b; }
vec4 xe_max4(vec4 a, vec4 b) { return mix(b, a, greaterThanEqual(a, b)); }
vec4 xe_min4(vec4 a, vec4 b) { return mix(b, a, lessThan(a, b)); }
float xe_dot(vec4 a, vec4 b, int n) {
    vec4 m = xe_mul4(a, b);
    float r = m.x + m.y;
    if (n > 2) r += m.z;
    if (n > 3) r += m.w;
    return r;
}
float xe_clamp_inf(float v, float pos, float neg) {
    return isinf(v) ? (v > 0.0 ? pos : neg) : v;
}
int xe_addr(float v, bool round_) {
    return int(floor(clamp(v, -256.0, 255.0) + (round_ ? 0.5 : 0.0)));
}
vec4 xe_cube(vec4 a) {
    // Operando .z_xy: x = a.z, y = a.w, z = a.x. Resultado: T, S, 2 * eixo maior, face.
    float x = a.z, y = a.w, z = a.x;
    vec4 r;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) {
        r = vec4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);
    } else if (abs(y) >= abs(x)) {
        r = vec4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);
    } else {
        r = vec4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);
    }
    r.z *= 2.0;
    return r;
}
float xe_max4h(vec4 a) {
    if (a.x >= a.y && a.x >= a.z && a.x >= a.w) return a.x;
    if (a.y >= a.z && a.y >= a.w) return a.y;
    return a.z >= a.w ? a.z : a.w;
}
uint xe_swap(uint v, uint e) {
    if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
    if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
    return v;
}
// Dword `addr` (em dwords do guest) do buffer da vertex fetch constant `fc`; 0 fora do buffer.
uint xe_vword(uint fc, uint addr) {
    uint d0 = xe_fetch(fc * 2u), d1 = xe_fetch(fc * 2u + 1u);
    uint base = d0 >> 2u;
    uint size = (d1 >> 2u) & 0xFFFFFFu;
    if (addr < base || addr - base >= size) return 0u;
    int delta = int(xe_u(XE_REMAP + fc));
    return xe_swap(xe_ring[uint(int(addr) + delta)], d1 & 3u);
}
float xe_unorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1u); }
float xe_snorm(int v, uint bits) { return max(-1.0, float(v) / float((1u << (bits - 1u)) - 1u)); }
float xe_snorm_nz(int v, uint bits) { return (float(v) + 0.5) * 2.0 / float((1u << bits) - 1u); }
// Gamma linear por partes do Xenos (PWL), como LinearToPWLGamma/PWLGammaToLinear de xenos.cc.
float xe_to_gamma(float v) {
    v = clamp(v, 0.0, 1.0);
    float scale, offset;
    if (v >= 512.0 / 1023.0) { scale = 1023.0 / 8.0; offset = 128.0 / 255.0; }
    else if (v >= 128.0 / 1023.0) { scale = 1023.0 / 4.0; offset = 64.0 / 255.0; }
    else if (v >= 64.0 / 1023.0) { scale = 1023.0 / 2.0; offset = 32.0 / 255.0; }
    else { scale = 1023.0; offset = 0.0; }
    return trunc(v * scale) * (1.0 / 255.0) + offset;
}
float xe_from_gamma(float g) {
    g = clamp(g, 0.0, 1.0);
    float scale, offset;
    if (g >= 192.0 / 255.0) { scale = 8.0 / 1024.0; offset = -1024.0; }
    else if (g >= 96.0 / 255.0) { scale = 4.0 / 1024.0; offset = -256.0; }
    else if (g >= 64.0 / 255.0) { scale = 2.0 / 1024.0; offset = -64.0; }
    else { scale = 1.0 / 1024.0; offset = 0.0; }
    float l = g * ((255.0 * 1024.0) * scale) + offset;
    l += trunc(l * scale);
    return l * (1.0 / 1023.0);
}
// Sinal dos componentes da textura (dword 0 da fetch constant): 2 = com bias (x * 2 - 1), 3 = gamma.
vec4 xe_tsign(vec4 v, uint td0) {
    for (int i = 0; i < 4; ++i) {
        uint s = (td0 >> (2u + 2u * uint(i))) & 3u;
        if (s == 2u) v[i] = v[i] * 2.0 - 1.0;
        else if (s == 3u) v[i] = xe_from_gamma(v[i]);
    }
    return v;
}
// Swizzle de destino da fetch constant de textura (3 bits por componente: xyzw, 0, 1).
vec4 xe_tswizzle(vec4 v, uint swz) {
    vec4 r;
    for (int i = 0; i < 4; ++i) {
        uint s = (swz >> (3u * uint(i))) & 7u;
        r[i] = s < 4u ? v[s] : (s == 5u ? 1.0 : 0.0);
    }
    return r;
}
)";
    }

    class Translator {
    public:
        Translator(const uint32_t* ucode, uint32_t dwordCount, xenos::ShaderType type, VertexShaderMode mode)
            : m_ucode(ucode), m_dwords(dwordCount), m_type(type), m_mode(mode) {
            m_out.type = type;
        }

        TranslatedShader Run() {
            if (!m_ucode || m_dwords < 3) {
                m_out.error = "microcodigo vazio";
                return m_out;
            }
            ScanControlFlow();
            EmitBody();
            if (!m_out.error.empty()) return m_out;
            Assemble();
            m_out.ok = true;
            return m_out;
        }

    private:
        bool IsPixel() const { return m_type == xenos::ShaderType::kPixel; }

        ControlFlowInstruction Cf(uint32_t index) const {
            ControlFlowInstruction ab[2];
            ucode::UnpackControlFlowInstructions(m_ucode + (index >> 1) * 3, ab);
            return ab[index & 1];
        }

        // Limite das instrucoes de controle de fluxo: o endereco da primeira exec (como no Xenia).
        void ScanControlFlow() {
            uint32_t pairBound = m_dwords / 3;
            for (uint32_t i = 0; i < pairBound; ++i) {
                for (uint32_t j = 0; j < 2; ++j) {
                    const ControlFlowInstruction cf = Cf(i * 2 + j);
                    if (ucode::IsControlFlowOpcodeExec(cf.opcode()) && cf.exec.count()) {
                        pairBound = std::min(pairBound, cf.exec.address());
                    }
                }
            }
            m_cfCount = pairBound * 2;
            for (uint32_t i = 0; i < m_cfCount; ++i) {
                const ControlFlowInstruction cf = Cf(i);
                switch (cf.opcode()) {
                case ControlFlowOpcode::kCondCall:
                    m_labels.insert(cf.cond_call.address());
                    m_labels.insert(i + 1);  // retorno
                    m_usesCalls = true;
                    break;
                case ControlFlowOpcode::kCondJmp: m_labels.insert(cf.cond_jmp.address()); break;
                case ControlFlowOpcode::kLoopStart: m_labels.insert(cf.loop_start.address()); break;
                case ControlFlowOpcode::kLoopEnd: m_labels.insert(cf.loop_end.address()); break;
                default: break;
                }
            }
        }

        // Instrucao sem traducao: vira no-op, e um comentario marca o motivo no GLSL.
        void Unsupported(const std::string& what) {
            ++m_out.unsupported;
            Line("// XE_UNSUPPORTED: " + what);
        }

        void Line(const std::string& s) {
            m_body.append(m_indent * 4, ' ');
            m_body += s;
            m_body += '\n';
        }

        std::string BoolCondition(uint32_t boolAddress, bool condition) const {
            return Format("xe_bool(%uu) == %s", boolAddress, condition ? "true" : "false");
        }

        void EmitBody() {
            m_indent = 2;
            for (uint32_t i = 0; i < m_cfCount; ++i) {
                if (i != 0 && m_labels.count(i)) {
                    m_indent = 1;
                    Line(Format("case %u:", i));
                    m_indent = 2;
                }
                EmitCf(i, Cf(i));
                if (!m_out.error.empty()) return;
            }
            Line("xe_pc = -1;");
            Line("break;");
        }

        void EmitCf(uint32_t index, const ControlFlowInstruction& cf) {
            switch (cf.opcode()) {
            case ControlFlowOpcode::kNop:
            case ControlFlowOpcode::kMarkVsFetchDone:
                break;

            case ControlFlowOpcode::kExec:
            case ControlFlowOpcode::kExecEnd:
            case ControlFlowOpcode::kCondExec:
            case ControlFlowOpcode::kCondExecEnd:
            case ControlFlowOpcode::kCondExecPred:
            case ControlFlowOpcode::kCondExecPredEnd:
            case ControlFlowOpcode::kCondExecPredClean:
            case ControlFlowOpcode::kCondExecPredCleanEnd: {
                std::string condition;
                switch (cf.opcode()) {
                case ControlFlowOpcode::kCondExec:
                case ControlFlowOpcode::kCondExecEnd:
                case ControlFlowOpcode::kCondExecPredClean:
                case ControlFlowOpcode::kCondExecPredCleanEnd:
                    condition = BoolCondition(cf.cond_exec.bool_address(), cf.cond_exec.condition());
                    break;
                case ControlFlowOpcode::kCondExecPred:
                case ControlFlowOpcode::kCondExecPredEnd:
                    condition = Format("xe_p0 == %s", cf.cond_exec_pred.condition() ? "true" : "false");
                    break;
                default: break;
                }
                Line(Format("// cf %u: exec %u instrucoes em %u", index, cf.exec.count(), cf.exec.address()));
                if (!condition.empty()) {
                    Line("if (" + condition + ") {");
                    ++m_indent;
                }
                const uint32_t sequence = cf.exec.sequence();
                for (uint32_t k = 0; k < cf.exec.count(); ++k) {
                    const uint32_t address = cf.exec.address() + k;
                    if ((address + 1) * 3 > m_dwords) {
                        m_out.error = Format("exec fora do microcodigo (cf %u)", index);
                        return;
                    }
                    const uint32_t* words = m_ucode + address * 3;
                    ++m_out.instructionCount;
                    if ((sequence >> (k * 2)) & 1) {
                        EmitFetch(*reinterpret_cast<const ucode::FetchInstruction*>(words));
                    } else {
                        EmitAlu(*reinterpret_cast<const AluInstruction*>(words));
                    }
                }
                if (ucode::DoesControlFlowOpcodeEndShader(cf.opcode())) {
                    Line("xe_pc = -1;");
                    Line("break;");
                }
                if (!condition.empty()) {
                    --m_indent;
                    Line("}");
                }
                break;
            }

            case ControlFlowOpcode::kLoopStart: {
                const auto& ls = cf.loop_start;
                Line(Format("// cf %u: loop_start il%u, fim em %u", index, ls.loop_id(), ls.address()));
                Line(Format("xe_lc = xe_loop(%uu);", ls.loop_id()));
                Line("xe_count = uvec4(xe_lc & 0xFFu, xe_count.xyz);");
                Line(Format("xe_aL = ivec4(%s, xe_aL.xyz);", ls.is_repeat() ? "xe_aL.x" : "int((xe_lc >> 8u) & 0xFFu)"));
                Line("if (xe_count.x == 0u) {");
                Line(Format("    xe_count = uvec4(xe_count.yzw, 0u); xe_aL = ivec4(xe_aL.yzw, 0); xe_pc = %u; continue;",
                            ls.address()));
                Line("}");
                break;
            }

            case ControlFlowOpcode::kLoopEnd: {
                const auto& le = cf.loop_end;
                Line(Format("// cf %u: loop_end il%u, corpo em %u", index, le.loop_id(), le.address()));
                Line("xe_count.x -= 1u;");
                std::string brk = "xe_count.x == 0u";
                if (le.is_predicated_break()) {
                    brk += Format(" || xe_p0 == %s", le.condition() ? "true" : "false");
                }
                Line("if (!(" + brk + ")) {");
                Line(Format("    xe_aL.x += bitfieldExtract(int(xe_loop(%uu)), 16, 8); xe_pc = %u; continue;",
                            le.loop_id(), le.address()));
                Line("}");
                Line("xe_count = uvec4(xe_count.yzw, 0u); xe_aL = ivec4(xe_aL.yzw, 0);");
                break;
            }

            case ControlFlowOpcode::kCondCall: {
                const auto& cc = cf.cond_call;
                std::string condition;
                if (!cc.is_unconditional()) {
                    condition = cc.is_predicated() ? Format("xe_p0 == %s", cc.condition() ? "true" : "false")
                                                   : BoolCondition(cc.bool_address(), cc.condition());
                }
                Line(Format("// cf %u: call %u", index, cc.address()));
                Line((condition.empty() ? std::string("if (xe_csp < 4)") : "if (xe_csp < 4 && " + condition + ")") + " {");
                Line(Format("    xe_calls[xe_csp++] = %u; xe_pc = %u; continue;", index + 1, cc.address()));
                Line("}");
                break;
            }

            case ControlFlowOpcode::kReturn:
                Line(Format("// cf %u: return", index));
                Line("if (xe_csp > 0) { xe_pc = xe_calls[--xe_csp]; continue; }");
                break;

            case ControlFlowOpcode::kCondJmp: {
                const auto& cj = cf.cond_jmp;
                std::string condition;
                if (!cj.is_unconditional()) {
                    condition = cj.is_predicated() ? Format("xe_p0 == %s", cj.condition() ? "true" : "false")
                                                   : BoolCondition(cj.bool_address(), cj.condition());
                }
                Line(Format("// cf %u: jmp %u", index, cj.address()));
                if (condition.empty()) {
                    Line(Format("xe_pc = %u; continue;", cj.address()));
                } else {
                    Line("if (" + condition + ") { xe_pc = " + std::to_string(cj.address()) + "; continue; }");
                }
                break;
            }

            case ControlFlowOpcode::kAlloc:
                if (cf.alloc.alloc_type() == ucode::AllocType::kMemory) m_out.usesMemexport = true;
                break;

            default:
                Unsupported(Format("cf opcode %u", uint32_t(cf.opcode())));
                Line(Format("// cf %u: opcode %u sem suporte", index, uint32_t(cf.opcode())));
                break;
            }
        }

        // ---------------------------------------------------------------- operandos e destinos

        std::string TempRegister(uint32_t index, bool relative) const {
            if (relative) return Format("xe_r[(%u + xe_aL.x) & 63]", index);
            return Format("xe_r[%u]", index & 63);
        }

        std::string ConstantRegister(uint32_t index, bool addressed, bool addressRegisterRelative) const {
            const uint32_t base = IsPixel() ? DrawBlock::kPixelFloatConstantBase : 0;
            if (!addressed) return Format("xe_c(%u)", base + index);
            return Format("xe_c(%u + %s)", base + index, addressRegisterRelative ? "xe_a0" : "xe_aL.x");
        }

        // Operando vetorial i (1..3) com swizzle, abs e negacao.
        std::string VectorOperand(const AluInstruction& op, uint32_t i) const {
            const uint32_t reg = op.src_reg(i);
            std::string value;
            bool absolute;
            if (op.src_is_temp(i)) {
                value = TempRegister(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
                absolute = AluInstruction::is_src_temp_value_absolute(reg);
            } else {
                value = ConstantRegister(reg, op.src_const_is_addressed(i), op.is_const_address_register_relative());
                absolute = op.abs_constants();
            }
            const uint32_t swizzle = op.src_swizzle(i);
            std::string sw = ".";
            for (uint32_t c = 0; c < 4; ++c) sw += kComponents[AluInstruction::GetSwizzledComponentIndex(swizzle, c)];
            if (sw != ".xyzw") value += sw;
            if (absolute) value = "abs(" + value + ")";
            if (op.src_negate(i)) value = "-" + value;
            return value;
        }

        // Operandos escalares: retorna a (W) e b (X ou Z) ja com modificadores.
        void ScalarOperands(const AluInstruction& op, uint32_t operandCount, bool twoComponent,
                            uint32_t vectorOperandCount, std::string& a, std::string& b) const {
            const uint32_t swizzle = op.src_swizzle(3);
            const bool negate = op.src_negate(3);
            auto finish = [&](std::string v, bool absolute) {
                if (absolute) v = "abs(" + v + ")";
                if (negate) v = "-" + v;
                return v;
            };
            if (operandCount == 1) {
                const uint32_t reg = op.src_reg(3);
                std::string source;
                bool absolute;
                if (op.src_is_temp(3)) {
                    source = TempRegister(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
                    absolute = AluInstruction::is_src_temp_value_absolute(reg);
                } else {
                    source = ConstantRegister(reg, op.src_const_is_addressed(3), op.is_const_address_register_relative());
                    absolute = op.abs_constants();
                }
                a = finish(source + "." + kComponents[AluInstruction::GetSwizzledComponentIndex(swizzle, 3)], absolute);
                if (twoComponent) {
                    const uint32_t second = vectorOperandCount == 3 ? 2 : 0;
                    b = finish(source + "." + kComponents[AluInstruction::GetSwizzledComponentIndex(swizzle, second)],
                               absolute);
                }
            } else if (operandCount == 2) {
                // Constante (W) e registrador temporario (X), as duas com abs_constants.
                const std::string constant =
                    ConstantRegister(op.src_reg(3), op.src_const_is_addressed(3), op.is_const_address_register_relative());
                a = finish(constant + "." + kComponents[AluInstruction::GetSwizzledComponentIndex(swizzle, 3)],
                           op.abs_constants());
                b = finish(TempRegister(op.scalar_const_reg_op_src_temp_reg(), false) + "." +
                               kComponents[AluInstruction::GetSwizzledComponentIndex(swizzle, 0)],
                           op.abs_constants());
            }
        }

        std::string MaskSwizzle(uint32_t mask) const {
            std::string s;
            for (uint32_t c = 0; c < 4; ++c) {
                if (mask & (1u << c)) s += kComponents[c];
            }
            return s;
        }

        // Destino de export: nome da variavel ou vazio (memexport e exports desconhecidos).
        std::string ExportTarget(uint32_t reg) {
            if (reg >= uint32_t(ExportRegister::kExportAddress) && reg <= uint32_t(ExportRegister::kExportData4)) {
                m_out.usesMemexport = true;
                return {};
            }
            if (!IsPixel()) {
                if (reg <= uint32_t(ExportRegister::kVSInterpolator15)) {
                    m_out.interpolatorsWritten |= 1u << reg;
                    return Format("o.interp[%u]", reg);
                }
                if (reg == uint32_t(ExportRegister::kVSPosition)) return "o.pos";
                if (reg == uint32_t(ExportRegister::kVSPointSizeEdgeFlagKillVertex)) {
                    m_out.writesPointSize = true;
                    return "o.psize";
                }
            } else {
                if (reg <= uint32_t(ExportRegister::kPSColor3)) {
                    m_out.colorTargetsWritten |= 1u << reg;
                    return Format("o.color[%u]", reg);
                }
                if (reg == uint32_t(ExportRegister::kPSDepth)) {
                    m_out.writesDepth = true;
                    return "o.depth";
                }
            }
            Unsupported(Format("export %u", reg));
            return {};
        }

        // ---------------------------------------------------------------- ALU

        void EmitAlu(const AluInstruction& op) {
            const AluVectorOpcode vop = op.vector_opcode();
            const AluScalarOpcode sop = op.scalar_opcode();
            const auto& vinfo = ucode::GetAluVectorOpcodeInfo(vop);
            const auto& sinfo = ucode::GetAluScalarOpcodeInfo(sop);
            const uint32_t vmask = op.GetVectorOpResultWriteMask();
            const uint32_t smask = op.GetScalarOpResultWriteMask();
            const uint32_t c0mask = op.GetConstant0WriteMask();
            const uint32_t c1mask = op.GetConstant1WriteMask();
            const bool vectorNeeded = vmask || vinfo.changed_state;
            const uint32_t vectorOperands = vinfo.GetOperandCount();

            std::string exportTarget;
            if (op.is_export()) {
                exportTarget = ExportTarget(op.vector_dest());
                if (exportTarget.empty() && !vinfo.changed_state && !sinfo.changed_state &&
                    sop == AluScalarOpcode::kRetainPrev) {
                    return;  // memexport sem efeito colateral visivel
                }
            } else if (!vectorNeeded && !smask && sop == AluScalarOpcode::kRetainPrev) {
                return;  // nop
            }

            Line("{");
            ++m_indent;
            if (op.is_predicated()) {
                Line(Format("if (xe_p0 == %s) {", op.predicate_condition() ? "true" : "false"));
                ++m_indent;
            }

            // Operandos lidos antes de qualquer escrita (vetor e escalar executam em paralelo).
            if (vectorNeeded) {
                for (uint32_t i = 0; i < vectorOperands; ++i) {
                    Line(Format("vec4 v%u = %s;", i, VectorOperand(op, i + 1).c_str()));
                }
            }
            std::string sa = "0.0", sb = "0.0";
            if (sinfo.operand_count) {
                ScalarOperands(op, sinfo.operand_count, sinfo.single_operand_is_two_component, vectorOperands, sa, sb);
            }
            Line("float sa = " + sa + ", sb = " + sb + ";");

            bool killVector = false;
            if (vectorNeeded) {
                std::string expr;
                bool replicate = false;
                switch (vop) {
                case AluVectorOpcode::kAdd: expr = "v0 + v1"; break;
                case AluVectorOpcode::kMul: expr = "xe_mul4(v0, v1)"; break;
                case AluVectorOpcode::kMax: expr = "xe_max4(v0, v1)"; break;
                case AluVectorOpcode::kMin: expr = "xe_min4(v0, v1)"; break;
                case AluVectorOpcode::kSeq: expr = "vec4(equal(v0, v1))"; break;
                case AluVectorOpcode::kSgt: expr = "vec4(greaterThan(v0, v1))"; break;
                case AluVectorOpcode::kSge: expr = "vec4(greaterThanEqual(v0, v1))"; break;
                case AluVectorOpcode::kSne: expr = "vec4(notEqual(v0, v1))"; break;
                case AluVectorOpcode::kFrc: expr = "v0 - floor(v0)"; break;
                case AluVectorOpcode::kTrunc: expr = "trunc(v0)"; break;
                case AluVectorOpcode::kFloor: expr = "floor(v0)"; break;
                case AluVectorOpcode::kMad: expr = "xe_mul4(v0, v1) + v2"; break;
                case AluVectorOpcode::kCndEq: expr = "mix(v2, v1, equal(v0, vec4(0.0)))"; break;
                case AluVectorOpcode::kCndGe: expr = "mix(v2, v1, greaterThanEqual(v0, vec4(0.0)))"; break;
                case AluVectorOpcode::kCndGt: expr = "mix(v2, v1, greaterThan(v0, vec4(0.0)))"; break;
                case AluVectorOpcode::kDp4: expr = "vec4(xe_dot(v0, v1, 4))"; break;
                case AluVectorOpcode::kDp3: expr = "vec4(xe_dot(v0, v1, 3))"; break;
                case AluVectorOpcode::kDp2Add: expr = "vec4(xe_dot(v0, v1, 2) + v2.x)"; break;
                case AluVectorOpcode::kCube: expr = "xe_cube(v0)"; break;
                case AluVectorOpcode::kMax4: expr = "vec4(xe_max4h(v0))"; break;
                case AluVectorOpcode::kSetpEqPush:
                    Line("xe_p0n = v0.w == 0.0 && v1.w == 0.0;");
                    expr = "vec4((v0.x == 0.0 && v1.x == 0.0) ? 0.0 : v0.x + 1.0)";
                    break;
                case AluVectorOpcode::kSetpNePush:
                    Line("xe_p0n = v0.w == 0.0 && v1.w != 0.0;");
                    expr = "vec4((v0.x == 0.0 && v1.x != 0.0) ? 0.0 : v0.x + 1.0)";
                    break;
                case AluVectorOpcode::kSetpGtPush:
                    Line("xe_p0n = v0.w == 0.0 && v1.w > 0.0;");
                    expr = "vec4((v0.x == 0.0 && v1.x > 0.0) ? 0.0 : v0.x + 1.0)";
                    break;
                case AluVectorOpcode::kSetpGePush:
                    Line("xe_p0n = v0.w == 0.0 && v1.w >= 0.0;");
                    expr = "vec4((v0.x == 0.0 && v1.x >= 0.0) ? 0.0 : v0.x + 1.0)";
                    break;
                case AluVectorOpcode::kKillEq: expr = "vec4(any(equal(v0, v1)) ? 1.0 : 0.0)"; killVector = true; break;
                case AluVectorOpcode::kKillGt: expr = "vec4(any(greaterThan(v0, v1)) ? 1.0 : 0.0)"; killVector = true; break;
                case AluVectorOpcode::kKillGe:
                    expr = "vec4(any(greaterThanEqual(v0, v1)) ? 1.0 : 0.0)";
                    killVector = true;
                    break;
                case AluVectorOpcode::kKillNe: expr = "vec4(any(notEqual(v0, v1)) ? 1.0 : 0.0)"; killVector = true; break;
                case AluVectorOpcode::kDst: expr = "vec4(1.0, xe_mul(v0.y, v1.y), v0.z, v1.w)"; break;
                case AluVectorOpcode::kMaxA:
                    Line("xe_a0n = xe_addr(v0.w, true);");
                    expr = "xe_max4(v0, v1)";
                    break;
                default:
                    Unsupported(Format("vector op %u", uint32_t(vop)));
                    expr = "vec4(0.0)";
                    break;
                }
                (void)replicate;
                Line("vec4 vr = " + expr + ";");
                if (op.vector_clamp()) Line("vr = clamp(vr, 0.0, 1.0);");
            }

            bool killScalar = false;
            std::string sexpr;
            switch (sop) {
            case AluScalarOpcode::kAdds:
            case AluScalarOpcode::kAddsc0:
            case AluScalarOpcode::kAddsc1: sexpr = "sa + sb"; break;
            case AluScalarOpcode::kAddsPrev: sexpr = "sa + xe_ps"; break;
            case AluScalarOpcode::kMuls:
            case AluScalarOpcode::kMulsc0:
            case AluScalarOpcode::kMulsc1: sexpr = "xe_mul(sa, sb)"; break;
            case AluScalarOpcode::kMulsPrev: sexpr = "xe_mul(sa, xe_ps)"; break;
            case AluScalarOpcode::kMulsPrev2:
                sexpr = "(xe_ps == -XE_FLT_MAX || isinf(xe_ps) || isnan(xe_ps) || isinf(sb) || isnan(sb) || sb <= 0.0) "
                        "? -XE_FLT_MAX : xe_mul(sa, xe_ps)";
                break;
            case AluScalarOpcode::kMaxs: sexpr = "xe_max(sa, sb)"; break;
            case AluScalarOpcode::kMins: sexpr = "xe_min(sa, sb)"; break;
            case AluScalarOpcode::kSeqs: sexpr = "float(sa == 0.0)"; break;
            case AluScalarOpcode::kSgts: sexpr = "float(sa > 0.0)"; break;
            case AluScalarOpcode::kSges: sexpr = "float(sa >= 0.0)"; break;
            case AluScalarOpcode::kSnes: sexpr = "float(sa != 0.0)"; break;
            case AluScalarOpcode::kFrcs: sexpr = "sa - floor(sa)"; break;
            case AluScalarOpcode::kTruncs: sexpr = "trunc(sa)"; break;
            case AluScalarOpcode::kFloors: sexpr = "floor(sa)"; break;
            case AluScalarOpcode::kExp: sexpr = "exp2(sa)"; break;
            case AluScalarOpcode::kLogc: sexpr = "xe_clamp_inf(log2(sa), XE_FLT_MAX, -XE_FLT_MAX)"; break;
            case AluScalarOpcode::kLog: sexpr = "log2(sa)"; break;
            case AluScalarOpcode::kRcpc: sexpr = "xe_clamp_inf(1.0 / sa, XE_FLT_MAX, -XE_FLT_MAX)"; break;
            case AluScalarOpcode::kRcpf: sexpr = "xe_clamp_inf(1.0 / sa, 0.0, -0.0)"; break;
            case AluScalarOpcode::kRcp: sexpr = "1.0 / sa"; break;
            case AluScalarOpcode::kRsqc: sexpr = "xe_clamp_inf(inversesqrt(sa), XE_FLT_MAX, -XE_FLT_MAX)"; break;
            case AluScalarOpcode::kRsqf: sexpr = "xe_clamp_inf(inversesqrt(sa), 0.0, -0.0)"; break;
            case AluScalarOpcode::kRsq: sexpr = "inversesqrt(sa)"; break;
            case AluScalarOpcode::kMaxAs:
                Line("xe_a0n = xe_addr(sa, true);");
                sexpr = "xe_max(sa, sb)";
                break;
            case AluScalarOpcode::kMaxAsf:
                Line("xe_a0n = xe_addr(sa, false);");
                sexpr = "xe_max(sa, sb)";
                break;
            case AluScalarOpcode::kSubs:
            case AluScalarOpcode::kSubsc0:
            case AluScalarOpcode::kSubsc1: sexpr = "sa - sb"; break;
            case AluScalarOpcode::kSubsPrev: sexpr = "sa - xe_ps"; break;
            case AluScalarOpcode::kSetpEq:
                Line("xe_p0n = sa == 0.0;");
                sexpr = "float(!xe_p0n)";
                break;
            case AluScalarOpcode::kSetpNe:
                Line("xe_p0n = sa != 0.0;");
                sexpr = "float(!xe_p0n)";
                break;
            case AluScalarOpcode::kSetpGt:
                Line("xe_p0n = sa > 0.0;");
                sexpr = "float(!xe_p0n)";
                break;
            case AluScalarOpcode::kSetpGe:
                Line("xe_p0n = sa >= 0.0;");
                sexpr = "float(!xe_p0n)";
                break;
            case AluScalarOpcode::kSetpInv:
                Line("xe_p0n = sa == 1.0;");
                sexpr = "xe_p0n ? 0.0 : (sa == 0.0 ? 1.0 : sa)";
                break;
            case AluScalarOpcode::kSetpPop:
                Line("xe_p0n = sa - 1.0 <= 0.0;");
                sexpr = "xe_p0n ? 0.0 : sa - 1.0";
                break;
            case AluScalarOpcode::kSetpClr:
                Line("xe_p0n = false;");
                sexpr = "XE_FLT_MAX";
                break;
            case AluScalarOpcode::kSetpRstr:
                Line("xe_p0n = sa == 0.0;");
                sexpr = "xe_p0n ? 0.0 : sa";
                break;
            case AluScalarOpcode::kKillsEq: sexpr = "float(sa == 0.0)"; killScalar = true; break;
            case AluScalarOpcode::kKillsGt: sexpr = "float(sa > 0.0)"; killScalar = true; break;
            case AluScalarOpcode::kKillsGe: sexpr = "float(sa >= 0.0)"; killScalar = true; break;
            case AluScalarOpcode::kKillsNe: sexpr = "float(sa != 0.0)"; killScalar = true; break;
            case AluScalarOpcode::kKillsOne: sexpr = "float(sa == 1.0)"; killScalar = true; break;
            case AluScalarOpcode::kSqrt: sexpr = "sqrt(sa)"; break;
            case AluScalarOpcode::kSin: sexpr = "sin(sa)"; break;
            case AluScalarOpcode::kCos: sexpr = "cos(sa)"; break;
            case AluScalarOpcode::kRetainPrev: sexpr = "xe_ps"; break;
            default:
                Unsupported(Format("scalar op %u", uint32_t(sop)));
                sexpr = "xe_ps";
                break;
            }
            Line("xe_ps = " + sexpr + ";");
            Line(std::string("float sr = ") + (op.scalar_clamp() ? "clamp(xe_ps, 0.0, 1.0)" : "xe_ps") + ";");

            // Estado alterado (predicado, a0) so depois de calcular os dois resultados.
            if (vinfo.changed_state & ucode::kAluOpChangedStatePredicate ||
                sinfo.changed_state & ucode::kAluOpChangedStatePredicate) {
                Line("xe_p0 = xe_p0n;");
            }
            if (vinfo.changed_state & ucode::kAluOpChangedStateAddressRegister ||
                sinfo.changed_state & ucode::kAluOpChangedStateAddressRegister) {
                Line("xe_a0 = xe_a0n;");
            }
            if (IsPixel() && (killVector || killScalar)) {
                m_out.killsPixels = true;
                if (killVector) Line("if (vr.x != 0.0) xe_kill = true;");
                if (killScalar) Line("if (xe_ps != 0.0) xe_kill = true;");
            }

            if (op.is_export()) {
                if (!exportTarget.empty()) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        const uint32_t bit = 1u << c;
                        std::string value;
                        if (vmask & bit) value = Format("vr.%c", kComponents[c]);
                        else if (smask & bit) value = "sr";
                        else if (c1mask & bit) value = "1.0";
                        else if (c0mask & bit) value = "0.0";
                        if (!value.empty()) Line(Format("%s.%c = %s;", exportTarget.c_str(), kComponents[c], value.c_str()));
                    }
                }
            } else {
                if (vmask) {
                    const std::string dest = TempRegister(op.vector_dest(), op.is_vector_dest_relative());
                    const std::string sw = MaskSwizzle(vmask);
                    Line(dest + "." + sw + " = vr." + sw + ";");
                }
                if (smask) {
                    const std::string dest = TempRegister(op.scalar_dest(), op.is_scalar_dest_relative());
                    const std::string sw = MaskSwizzle(smask);
                    Line(dest + "." + sw + (sw.size() == 1 ? " = sr;" : Format(" = vec%u(sr);", uint32_t(sw.size()))));
                }
            }

            if (op.is_predicated()) {
                --m_indent;
                Line("}");
            }
            --m_indent;
            Line("}");
        }

        // ---------------------------------------------------------------- fetch

        void StoreFetchResult(uint32_t dest, bool relative, uint32_t swizzle, const char* value) {
            const std::string target = TempRegister(dest, relative);
            for (uint32_t c = 0; c < 4; ++c) {
                const auto s = ucode::GetFetchDestinationComponentSwizzle(swizzle, c);
                switch (s) {
                case ucode::FetchDestinationSwizzle::kKeep: break;
                case ucode::FetchDestinationSwizzle::kX:
                case ucode::FetchDestinationSwizzle::kY:
                case ucode::FetchDestinationSwizzle::kZ:
                case ucode::FetchDestinationSwizzle::kW:
                    Line(Format("%s.%c = %s.%c;", target.c_str(), kComponents[c], value, kComponents[uint32_t(s)]));
                    break;
                case ucode::FetchDestinationSwizzle::k1:
                    Line(Format("%s.%c = 1.0;", target.c_str(), kComponents[c]));
                    break;
                default:
                    Line(Format("%s.%c = 0.0;", target.c_str(), kComponents[c]));
                    break;
                }
            }
        }

        void EmitFetch(const ucode::FetchInstruction& op) {
            Line("{");
            ++m_indent;
            if (op.is_predicated()) {
                Line(Format("if (xe_p0 == %s) {", op.predicate_condition() ? "true" : "false"));
                ++m_indent;
            }
            if (op.opcode() == FetchOpcode::kVertexFetch) {
                EmitVertexFetch(op.vertex_fetch());
            } else {
                EmitTextureFetch(op.texture_fetch());
            }
            if (op.is_predicated()) {
                --m_indent;
                Line("}");
            }
            --m_indent;
            Line("}");
        }

        void EmitVertexFetch(const ucode::VertexFetchInstruction& op) {
            if (!op.is_mini_fetch()) {
                m_lastFullFetch = op;
                m_hasFullFetch = true;
            }
            if (!m_hasFullFetch) {
                Unsupported("vfetch mini sem full");
                return;
            }
            const uint32_t fc = m_lastFullFetch.fetch_constant_index();
            m_out.vertexFetchMask[fc >> 5] |= 1u << (fc & 31);
            if (!op.is_mini_fetch()) {
                const std::string src = TempRegister(op.src(), op.is_src_relative());
                Line(Format("xe_vfa = %uu * uint(int(floor(%s.%c%s))) + (xe_fetch(%uu) >> 2u);", op.stride(), src.c_str(),
                            kComponents[op.src_swizzle() & 3], op.is_index_rounded() ? " + 0.5" : "", fc * 2));
            }
            Line(Format("uint xa = uint(int(xe_vfa) + %d);", op.offset()));

            const xenos::VertexFormat format = op.data_format();
            const bool sgn = op.is_signed();
            const bool norm = op.is_normalized();
            const bool noZero = op.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero;
            // Componentes empacotados: largura e offset em bits, e dword de origem.
            struct Packed { uint32_t width, offset, word; };
            Packed packed[4] = {};
            uint32_t packedCount = 0;
            uint32_t words = 0;
            switch (format) {
            case xenos::VertexFormat::k_8_8_8_8:
                packed[0] = { 8, 0, 0 }; packed[1] = { 8, 8, 0 }; packed[2] = { 8, 16, 0 }; packed[3] = { 8, 24, 0 };
                packedCount = 4; words = 1;
                break;
            case xenos::VertexFormat::k_2_10_10_10:
                packed[0] = { 10, 0, 0 }; packed[1] = { 10, 10, 0 }; packed[2] = { 10, 20, 0 }; packed[3] = { 2, 30, 0 };
                packedCount = 4; words = 1;
                break;
            case xenos::VertexFormat::k_10_11_11:
                packed[0] = { 11, 0, 0 }; packed[1] = { 11, 11, 0 }; packed[2] = { 10, 22, 0 };
                packedCount = 3; words = 1;
                break;
            case xenos::VertexFormat::k_11_11_10:
                packed[0] = { 10, 0, 0 }; packed[1] = { 11, 10, 0 }; packed[2] = { 11, 21, 0 };
                packedCount = 3; words = 1;
                break;
            case xenos::VertexFormat::k_16_16:
                packed[0] = { 16, 0, 0 }; packed[1] = { 16, 16, 0 };
                packedCount = 2; words = 1;
                break;
            case xenos::VertexFormat::k_16_16_16_16:
                packed[0] = { 16, 0, 0 }; packed[1] = { 16, 16, 0 }; packed[2] = { 16, 0, 1 }; packed[3] = { 16, 16, 1 };
                packedCount = 4; words = 2;
                break;
            case xenos::VertexFormat::k_16_16_FLOAT: words = 1; break;
            case xenos::VertexFormat::k_16_16_16_16_FLOAT: words = 2; break;
            case xenos::VertexFormat::k_32:
            case xenos::VertexFormat::k_32_FLOAT: words = 1; break;
            case xenos::VertexFormat::k_32_32:
            case xenos::VertexFormat::k_32_32_FLOAT: words = 2; break;
            case xenos::VertexFormat::k_32_32_32_FLOAT: words = 3; break;
            case xenos::VertexFormat::k_32_32_32_32:
            case xenos::VertexFormat::k_32_32_32_32_FLOAT: words = 4; break;
            case xenos::VertexFormat::kUndefined: break;  // nao le memoria (como no Xenia)
            default:
                Unsupported(Format("vertex format %u", uint32_t(format)));
                break;
            }
            Line("vec4 vf = vec4(0.0, 0.0, 0.0, 1.0);");
            for (uint32_t w = 0; w < words; ++w) Line(Format("uint w%u = xe_vword(%uu, xa + %uu);", w, fc, w));
            if (packedCount) {
                for (uint32_t c = 0; c < packedCount; ++c) {
                    const Packed& p = packed[c];
                    std::string v;
                    if (sgn) {
                        const std::string raw = Format("bitfieldExtract(int(w%u), %u, %u)", p.word, p.offset, p.width);
                        v = norm ? Format("%s(%s, %uu)", noZero ? "xe_snorm_nz" : "xe_snorm", raw.c_str(), p.width)
                                 : "float(" + raw + ")";
                    } else {
                        const std::string raw = Format("bitfieldExtract(w%u, %u, %u)", p.word, p.offset, p.width);
                        v = norm ? Format("xe_unorm(%s, %uu)", raw.c_str(), p.width) : "float(" + raw + ")";
                    }
                    Line(Format("vf.%c = %s;", kComponents[c], v.c_str()));
                }
            } else {
                switch (format) {
                case xenos::VertexFormat::k_16_16_FLOAT:
                    Line("vf.xy = unpackHalf2x16(w0);");
                    break;
                case xenos::VertexFormat::k_16_16_16_16_FLOAT:
                    Line("vf = vec4(unpackHalf2x16(w0), unpackHalf2x16(w1));");
                    break;
                case xenos::VertexFormat::k_32_FLOAT:
                case xenos::VertexFormat::k_32_32_FLOAT:
                case xenos::VertexFormat::k_32_32_32_FLOAT:
                case xenos::VertexFormat::k_32_32_32_32_FLOAT:
                    for (uint32_t w = 0; w < words; ++w) Line(Format("vf.%c = uintBitsToFloat(w%u);", kComponents[w], w));
                    break;
                case xenos::VertexFormat::k_32:
                case xenos::VertexFormat::k_32_32:
                case xenos::VertexFormat::k_32_32_32_32:
                    for (uint32_t w = 0; w < words; ++w) {
                        std::string v = sgn ? Format("float(int(w%u))", w) : Format("float(w%u)", w);
                        if (norm) {
                            v = sgn ? (noZero ? "(" + v + " + 0.5) / 2147483647.5" : v + " / 2147483647.0")
                                    : v + " / 4294967295.0";
                        }
                        Line(Format("vf.%c = %s;", kComponents[w], v.c_str()));
                    }
                    break;
                default: break;
                }
            }
            if (op.exp_adjust()) Line(Format("vf *= exp2(%d.0);", op.exp_adjust()));
            StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "vf");
        }

        uint32_t FindOrAddTexture(const TextureBinding& binding) {
            for (uint32_t i = 0; i < m_out.textures.size(); ++i) {
                if (m_out.textures[i] == binding) return i;
            }
            m_out.textures.push_back(binding);
            return uint32_t(m_out.textures.size() - 1);
        }

        void EmitTextureFetch(const ucode::TextureFetchInstruction& op) {
            const FetchOpcode opcode = op.opcode();
            const std::string src = TempRegister(op.src(), op.is_src_relative());
            auto srcComponent = [&](uint32_t i) {
                return Format("%s.%c", src.c_str(), kComponents[(op.src_swizzle() >> (i * 2)) & 3]);
            };

            switch (opcode) {
            case FetchOpcode::kSetTextureLod:
                Line("xe_tlod = " + srcComponent(0) + ";");
                return;
            case FetchOpcode::kSetTextureGradientsHorz:
                Line("xe_tgradh = vec3(" + srcComponent(0) + ", " + srcComponent(1) + ", " + srcComponent(2) + ");");
                return;
            case FetchOpcode::kSetTextureGradientsVert:
                Line("xe_tgradv = vec3(" + srcComponent(0) + ", " + srcComponent(1) + ", " + srcComponent(2) + ");");
                return;
            case FetchOpcode::kGetTextureGradients:
                if (IsPixel()) {
                    Line("vec4 tr = vec4(dFdx(" + srcComponent(0) + "), dFdy(" + srcComponent(0) + "), dFdx(" +
                         srcComponent(1) + "), dFdy(" + srcComponent(1) + "));");
                } else {
                    Line("vec4 tr = vec4(0.0);");
                }
                StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tr");
                return;
            case FetchOpcode::kTextureFetch:
            case FetchOpcode::kGetTextureComputedLod:
            case FetchOpcode::kGetTextureWeights:
            case FetchOpcode::kGetTextureBorderColorFrac:
                break;
            default:
                Unsupported(Format("fetch opcode %u", uint32_t(opcode)));
                return;
            }

            if (opcode == FetchOpcode::kGetTextureBorderColorFrac) {
                Line("vec4 tr = vec4(0.0);");
                StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tr");
                return;
            }

            const uint32_t fc = op.fetch_constant_index();
            xenos::FetchOpDimension dimension = op.dimension();
            uint32_t coordCount = xenos::GetFetchOpDimensionComponentCount(dimension);
            if (dimension == xenos::FetchOpDimension::k1D) {
                // 1D com swizzles diferentes fornece XY para uma fetch constant 2D (como no Xenia).
                const uint32_t sw = op.src_swizzle();
                if (((sw >> 2) & 3) != (sw & 3) || ((sw >> 4) & 3) != (sw & 3)) coordCount = 2;
            }

            TextureBinding binding;
            binding.fetchConstant = fc;
            binding.dimension = dimension == xenos::FetchOpDimension::k1D ? xenos::FetchOpDimension::k2D : dimension;
            binding.magFilter = op.mag_filter();
            binding.minFilter = op.min_filter();
            binding.mipFilter = op.mip_filter();
            binding.anisoFilter = op.aniso_filter();
            const uint32_t t = FindOrAddTexture(binding);
            const uint32_t fcWord = fc * 6;

            // Coordenadas e tamanho da textura (dword 2 da fetch constant; dword 5 bits 9:10 = dimensao).
            Line("vec3 tc = vec3(" + srcComponent(0) + ", " + (coordCount > 1 ? srcComponent(1) : "0.0") + ", " +
                 (coordCount > 2 ? srcComponent(2) : "0.0") + ");");
            Line(Format("uint td2 = xe_fetch(%uu), td3 = xe_fetch(%uu), td4 = xe_fetch(%uu), td5 = xe_fetch(%uu);",
                        fcWord + 2, fcWord + 3, fcWord + 4, fcWord + 5));
            Line("uint tdim = (td5 >> 9u) & 3u;");
            Line("vec3 tsize = tdim == 2u ? vec3(float((td2 & 0x7FFu) + 1u), float(((td2 >> 11u) & 0x7FFu) + 1u), "
                 "float((td2 >> 22u) + 1u)) : vec3(float((td2 & 0x1FFFu) + 1u), float(((td2 >> 13u) & 0x1FFFu) + 1u), "
                 "float((td2 >> 26u) + 1u));");
            if (tdimIs1D(dimension, coordCount)) {
                Line("if (tdim == 0u) tsize = vec3(float((td2 & 0xFFFFFFu) + 1u), 1.0, 1.0);");
            }

            const float offsets[3] = { op.offset_x(), op.offset_y(), op.offset_z() };
            const bool unnormalized = op.unnormalized_coordinates();

            if (opcode == FetchOpcode::kGetTextureWeights) {
                // Fracao das coordenadas em texels (ignorando o LOD, como o Xenia).
                std::string coords = unnormalized ? "tc" : "tc * tsize";
                Line(Format("vec3 tw = fract(%s + vec3(%.6f, %.6f, %.6f) - 0.5);", coords.c_str(), offsets[0], offsets[1],
                            offsets[2]));
                Line(Format("vec4 tr = vec4(tw.x, %s, %s, 0.0);", coordCount > 1 ? "tw.y" : "0.0",
                            coordCount > 2 ? "tw.z" : "0.0"));
                StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tr");
                return;
            }

            // Normalizacao e offsets (em texels, granularidade 0.5).
            const uint32_t xyCount = dimension == xenos::FetchOpDimension::kCube ? 2 : std::min(coordCount, 2u);
            for (uint32_t i = 0; i < xyCount; ++i) {
                const char c = kComponents[i];
                if (unnormalized) {
                    Line(Format("tc.%c = (tc.%c + %.6f) / tsize.%c;", c, c, offsets[i], c));
                } else if (offsets[i] != 0.0f) {
                    Line(Format("tc.%c += %.6f / tsize.%c;", c, offsets[i], c));
                }
            }

            std::string lodExpr;
            const float lodBias = op.lod_bias();
            // LOD: implicito (so no PS), ou do registrador (setTexLOD), mais os biases.
            const bool implicitLod = IsPixel() && op.use_computed_lod() && !op.use_register_lod();
            Line(Format("float tbias = %.6f + float(bitfieldExtract(int(td4), 12, 10)) / 32.0;", lodBias));
            if (!implicitLod) {
                lodExpr = op.use_register_lod() ? "xe_tlod + tbias" : "tbias";
            }

            auto sample = [&](const std::string& samplerExpr, const std::string& coord) {
                if (opcode == FetchOpcode::kGetTextureComputedLod) {
                    return IsPixel() ? "vec4(textureQueryLod(" + samplerExpr + ", " + coord + ").y + tbias)" : std::string("vec4(0.0)");
                }
                if (op.use_register_gradients() && IsPixel()) {
                    return "textureGrad(" + samplerExpr + ", " + coord + ", xe_tgradh_c, xe_tgradv_c)";
                }
                if (implicitLod) return "texture(" + samplerExpr + ", " + coord + ", tbias)";
                return "textureLod(" + samplerExpr + ", " + coord + ", " + lodExpr + ")";
            };

            switch (dimension) {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D: {
                if (op.use_register_gradients()) Line("vec2 xe_tgradh_c = xe_tgradh.xy, xe_tgradv_c = xe_tgradv.xy;");
                Line("vec4 tr = " + sample(Format("sampler2D(xe_t%u, xe_s%u)", t, t), "tc.xy") + ";");
                break;
            }
            case xenos::FetchOpDimension::k3DOrStacked: {
                // 3D (tdim == 2) ou 2D empilhada (array): o host liga as duas imagens.
                Line("vec4 tr;");
                Line("if (tdim == 2u) {");
                ++m_indent;
                if (unnormalized) Line(Format("tc.z = (tc.z + %.6f) / tsize.z;", offsets[2]));
                else if (offsets[2] != 0.0f) Line(Format("tc.z += %.6f / tsize.z;", offsets[2]));
                if (op.use_register_gradients()) Line("vec3 xe_tgradh_c = xe_tgradh, xe_tgradv_c = xe_tgradv;");
                Line("tr = " + sample(Format("sampler3D(xe_t%u, xe_s%u)", t, t), "tc") + ";");
                --m_indent;
                Line("} else {");
                ++m_indent;
                Line(Format("float layer = clamp(%s + %.6f, 0.0, tsize.z - 1.0);", unnormalized ? "tc.z" : "tc.z * tsize.z",
                            offsets[2]));
                if (op.use_register_gradients()) Line("vec2 xe_tgradh_c = xe_tgradh.xy, xe_tgradv_c = xe_tgradv.xy;");
                Line("tr = " + sample(Format("sampler2DArray(xe_ta%u, xe_s%u)", t, t), "vec3(tc.xy, floor(layer + 0.5))") + ";");
                --m_indent;
                Line("}");
                break;
            }
            case xenos::FetchOpDimension::kCube: {
                // SC/TC de 1..2 para -1..1 e face 0..5 (inverso da instrucao cube da ALU).
                Line("vec2 cst = tc.xy * 2.0 - 3.0;");
                Line(Format("uint face = uint(clamp(tc.z + %.6f, 0.0, 5.0));", offsets[2]));
                Line("float fsign = (face & 1u) != 0u ? -1.0 : 1.0;");
                Line("bool fneg = (face & 1u) != 0u;");
                Line("vec3 dir;");
                Line("if ((face >> 1u) == 0u) dir = vec3(fsign, -cst.y, fneg ? cst.x : -cst.x);");
                Line("else if ((face >> 1u) == 1u) dir = vec3(cst.x, fsign, fneg ? -cst.y : cst.y);");
                Line("else dir = vec3(fneg ? -cst.x : cst.x, -cst.y, fsign);");
                if (op.use_register_gradients()) Line("vec3 xe_tgradh_c = xe_tgradh, xe_tgradv_c = xe_tgradv;");
                Line("vec4 tr = " + sample(Format("samplerCube(xe_t%u, xe_s%u)", t, t), "dir") + ";");
                break;
            }
            }

            if (opcode == FetchOpcode::kTextureFetch) {
                // Sinal (dword 0), swizzle e expoente (dword 3) da fetch constant.
                Line(Format("tr = xe_tsign(tr, xe_fetch(%uu));", fcWord));
                Line("tr = xe_tswizzle(tr, (td3 >> 1u) & 0xFFFu);");
                Line("tr *= exp2(float(bitfieldExtract(int(td3), 13, 6)));");
            }
            StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tr");
        }

        static bool tdimIs1D(xenos::FetchOpDimension dimension, uint32_t coordCount) {
            return dimension == xenos::FetchOpDimension::k1D && coordCount == 1;
        }

        // ---------------------------------------------------------------- montagem

        std::string Declarations() const {
            std::string s;
            const uint32_t base = IsPixel() ? kPixelTextureBindingBase : 0;
            for (uint32_t i = 0; i < m_out.textures.size(); ++i) {
                const TextureBinding& b = m_out.textures[i];
                const char* imageType = "texture2D";
                if (b.dimension == xenos::FetchOpDimension::k3DOrStacked) imageType = "texture3D";
                if (b.dimension == xenos::FetchOpDimension::kCube) imageType = "textureCube";
                s += Format("layout(set = 1, binding = %u) uniform %s xe_t%u;\n", base + i * 3, imageType, i);
                s += Format("layout(set = 1, binding = %u) uniform sampler xe_s%u;\n", base + i * 3 + 1, i);
                if (b.dimension == xenos::FetchOpDimension::k3DOrStacked) {
                    s += Format("layout(set = 1, binding = %u) uniform texture2DArray xe_ta%u;\n", base + i * 3 + 2, i);
                }
            }
            return s;
        }

        // Corpo do programa do guest como funcao: o VS de rect list a chama 3 vezes por vertice.
        std::string GuestFunction() const {
            std::string s;
            if (IsPixel()) {
                s += "struct XeOut { vec4 color[4]; vec4 depth; };\n";
                s += "void xe_guest(inout vec4 xe_r[64], inout XeOut o, inout bool xe_kill) {\n";
            } else {
                s += "struct XeOut { vec4 pos; vec4 interp[16]; vec4 psize; };\n";
                s += "void xe_guest(inout vec4 xe_r[64], inout XeOut o) {\n";
            }
            s += "    int xe_pc = 0;\n";
            s += "    bool xe_p0 = false, xe_p0n = false;\n";
            s += "    int xe_a0 = 0, xe_a0n = 0;\n";
            s += "    ivec4 xe_aL = ivec4(0);\n";
            s += "    uvec4 xe_count = uvec4(0u);\n";
            s += "    uint xe_lc = 0u;\n";
            s += "    float xe_ps = 0.0;\n";
            s += "    uint xe_vfa = 0u;\n";
            s += "    float xe_tlod = 0.0;\n";
            s += "    vec3 xe_tgradh = vec3(0.0), xe_tgradv = vec3(0.0);\n";
            if (m_usesCalls) s += "    int xe_calls[4]; int xe_csp = 0;\n";
            s += "    while (xe_pc != -1) {\n";
            s += "    switch (xe_pc) {\n";
            s += "    case 0:\n";
            s += m_body;
            s += "    default:\n";
            s += "        xe_pc = -1;\n";
            s += "        break;\n";
            s += "    }\n";
            s += "    }\n";
            s += "}\n";
            return s;
        }

        void Assemble() {
            std::string g = "#version 450\n";
            g += Format("#define XE_BOOL %uu\n#define XE_LOOP %uu\n#define XE_FETCH %uu\n#define XE_REMAP %uu\n#define XE_SYS %uu\n",
                        DrawBlock::kBoolConstants, DrawBlock::kLoopConstants, DrawBlock::kFetchConstants,
                        DrawBlock::kVertexRemap, DrawBlock::kSystem);
            g += kCommonPrelude;
            g += Declarations();
            g += GuestFunction();
            g += IsPixel() ? PixelMain() : VertexMain();
            m_out.glsl = std::move(g);
        }

        std::string VertexMain() const {
            std::string s = Format(R"(
layout(location = 0) out vec4 xe_interp_out[16];
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };

const uint XE_FLAGS = %uu;
uint xe_guest_index(uint host_index) {
    uint flags = xe_sys(XE_FLAGS);
    uint index = host_index;
    if ((flags & %uu) != 0u) index = xe_ring[xe_sys(%uu) + host_index];
    index = (index + xe_sys(%uu)) & 0xFFFFFFu;
    return clamp(index, xe_sys(%uu), xe_sys(%uu));
}
XeOut xe_run(uint host_index) {
    vec4 xe_r[64];
    for (int i = 0; i < 64; ++i) xe_r[i] = vec4(0.0);
    xe_r[0].x = float(xe_guest_index(host_index));
    XeOut o;
    o.pos = vec4(0.0, 0.0, 0.0, 1.0);
    for (int i = 0; i < 16; ++i) o.interp[i] = vec4(0.0);
    o.psize = vec4(0.0);
    xe_guest(xe_r, o);
    return o;
}
)",
                                   Sys::kFlags, Sys::kFlagIndicesFromRing, Sys::kIndexRingOffset, Sys::kVertexIndexOffset,
                                   Sys::kVertexIndexMin, Sys::kVertexIndexMax);
            if (m_mode == VertexShaderMode::kRectangleList) {
                // 6 vertices por retangulo: (0,1,2) e (a,b,3), com o 4o canto extrapolado do
                // vertice oposto a maior aresta (a diagonal), como o geometry shader do Xenia.
                s += R"(
void main() {
    uint rect = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    XeOut o;
    if (corner < 3u) {
        o = xe_run(rect * 3u + corner);
    } else {
        XeOut v0 = xe_run(rect * 3u), v1 = xe_run(rect * 3u + 1u), v2 = xe_run(rect * 3u + 2u);
        vec2 p0 = v0.pos.xy / v0.pos.w, p1 = v1.pos.xy / v1.pos.w, p2 = v2.pos.xy / v2.pos.w;
        float d01 = dot(p1 - p0, p1 - p0), d02 = dot(p2 - p0, p2 - p0), d12 = dot(p2 - p1, p2 - p1);
        // Diagonal (a, b) e canto c; o 4o vertice e a + b - c.
        XeOut a = v1, b = v2, c = v0;
        if (d02 > d01 && d02 >= d12) { a = v0; b = v2; c = v1; }
        else if (d01 >= d02 && d01 >= d12) { a = v0; b = v1; c = v2; }
        if (corner == 3u) o = a;
        else if (corner == 4u) o = b;
        else {
            o.pos = a.pos + b.pos - c.pos;
            for (int i = 0; i < 16; ++i) o.interp[i] = a.interp[i] + b.interp[i] - c.interp[i];
            o.psize = a.psize;
        }
    }
)";
            } else {
                s += R"(
void main() {
    XeOut o = xe_run(uint(gl_VertexIndex));
)";
            }
            s += Format(R"(
    for (int i = 0; i < 16; ++i) xe_interp_out[i] = o.interp[i];
    // Transformacao de viewport do Xenos para NDC do host (PA_CL_VTE_CNTL / draw_util do Xenia).
    uint flags = xe_sys(XE_FLAGS);
    vec4 p = o.pos;
    float w = (flags & %uu) != 0u ? p.w : 1.0 / p.w;
    vec2 xy = (flags & %uu) != 0u ? p.xy * w : p.xy;
    float z = (flags & %uu) != 0u ? p.z * w : p.z;
    vec3 ndcScale = vec3(xe_sysf(%uu), xe_sysf(%uu), xe_sysf(%uu));
    vec3 ndcOffset = vec3(xe_sysf(%uu), xe_sysf(%uu), xe_sysf(%uu));
    gl_Position = vec4(vec3(xy, z) * ndcScale + ndcOffset * w, w);
    float psize = %s;
    vec2 pminmax = vec2(xe_sysf(%uu), xe_sysf(%uu));
    gl_PointSize = clamp(psize, pminmax.x, pminmax.y);
}
)",
                        Sys::kFlagWNotReciprocal, Sys::kFlagXYDividedByW, Sys::kFlagZDividedByW, Sys::kNdcScale,
                        Sys::kNdcScale + 1, Sys::kNdcScale + 2, Sys::kNdcOffset, Sys::kNdcOffset + 1, Sys::kNdcOffset + 2,
                        m_out.writesPointSize ? "o.psize.x > 0.0 ? o.psize.x : xe_sysf(7u)" : "xe_sysf(7u)",
                        Sys::kPointSizeMinMax, Sys::kPointSizeMinMax + 1);
            return s;
        }

        std::string PixelMain() const {
            std::string s = Format(R"(
layout(location = 0) in vec4 xe_interp_in[16];
layout(location = 0) out vec4 xe_color0;
layout(location = 1) out vec4 xe_color1;
layout(location = 2) out vec4 xe_color2;
layout(location = 3) out vec4 xe_color3;

vec4 xe_to_gamma4(vec4 c) { return vec4(xe_to_gamma(c.r), xe_to_gamma(c.g), xe_to_gamma(c.b), c.a); }

bool xe_alpha_test(float a) {
    float ref = xe_sysf(%uu);
    switch (xe_sys(%uu)) {
    case 0u: return false;
    case 1u: return a < ref;
    case 2u: return a == ref;
    case 3u: return a <= ref;
    case 4u: return a > ref;
    case 5u: return a != ref;
    case 6u: return a >= ref;
    default: return true;
    }
}

void main() {
    vec4 xe_r[64];
    for (int i = 0; i < 64; ++i) xe_r[i] = i < 16 ? xe_interp_in[i] : vec4(0.0);
    // ps_param_gen: posicao do pixel (sinal de X = face traseira), sinal de Z = linha.
    uint paramGen = xe_sys(%uu);
    if (paramGen < 64u) {
        uint flags = xe_sys(%uu);
        vec2 wo = vec2(xe_sysf(%uu), xe_sysf(%uu));
        float px = abs(floor(gl_FragCoord.x)) + wo.x;
        float py = abs(floor(gl_FragCoord.y)) + wo.y;
        if ((flags & %uu) != 0u && !gl_FrontFacing) px = -px;
        if ((flags & %uu) != 0u) py = -py;
        float pz = (flags & %uu) != 0u ? -0.0 : 0.0;
        vec2 pc = (flags & %uu) != 0u ? clamp(gl_PointCoord, 0.0, 1.0) : vec2(pz, 0.0);
        xe_r[paramGen] = vec4(px, py, pc);
    }
    XeOut o;
    for (int i = 0; i < 4; ++i) o.color[i] = vec4(0.0);
    o.depth = vec4(gl_FragCoord.z);
    bool xe_kill = false;
    xe_guest(xe_r, o, xe_kill);
    if (xe_kill) discard;
    uint flags = xe_sys(%uu);
    if ((flags & %uu) != 0u && !xe_alpha_test(o.color[0].a)) discard;
    vec4 bias = vec4(xe_sysf(%uu), xe_sysf(%uu), xe_sysf(%uu), xe_sysf(%uu));
    uint gammaTargets = xe_sys(%uu);
    vec4 c0 = o.color[0] * bias.x, c1 = o.color[1] * bias.y, c2 = o.color[2] * bias.z, c3 = o.color[3] * bias.w;
    xe_color0 = (gammaTargets & 1u) != 0u ? xe_to_gamma4(c0) : c0;
    xe_color1 = (gammaTargets & 2u) != 0u ? xe_to_gamma4(c1) : c1;
    xe_color2 = (gammaTargets & 4u) != 0u ? xe_to_gamma4(c2) : c2;
    xe_color3 = (gammaTargets & 8u) != 0u ? xe_to_gamma4(c3) : c3;
)",
                                   Sys::kAlphaRef, Sys::kAlphaFunc, Sys::kParamGenRegister, Sys::kFlags,
                                   Sys::kParamGenWindowOffset, Sys::kParamGenWindowOffset + 1, Sys::kFlagPrimitivePolygonal,
                                   Sys::kFlagPrimitivePoint, Sys::kFlagPrimitiveLine, Sys::kFlagPrimitivePoint, Sys::kFlags,
                                   Sys::kFlagAlphaTest, Sys::kColorExpBias, Sys::kColorExpBias + 1, Sys::kColorExpBias + 2,
                                   Sys::kColorExpBias + 3, Sys::kGammaTargets);
            if (m_out.writesDepth) s += "    gl_FragDepth = clamp(o.depth.x, 0.0, 1.0);\n";
            s += "}\n";
            return s;
        }

        const uint32_t* m_ucode;
        uint32_t m_dwords;
        xenos::ShaderType m_type;
        VertexShaderMode m_mode;
        TranslatedShader m_out;

        uint32_t m_cfCount = 0;
        std::set<uint32_t> m_labels;
        bool m_usesCalls = false;
        std::string m_body;
        int m_indent = 0;

        ucode::VertexFetchInstruction m_lastFullFetch{};
        bool m_hasFullFetch = false;
    };

    TranslatedShader TranslateShader(const uint32_t* ucode, uint32_t dwordCount, xenos::ShaderType type,
                                     VertexShaderMode vertexMode) {
        return Translator(ucode, dwordCount, type, vertexMode).Run();
    }
}
