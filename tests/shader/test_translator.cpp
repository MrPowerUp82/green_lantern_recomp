#include "test_harness.h"

#include "gpu/shader/glsl_translator.h"
#include "gpu/shader/spirv_compiler.h"
#include "gpu/shader/ucode_builder.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace Gpu::Shader;
namespace U = Gpu::Shader::Ucode;
using xe::gpu::ucode::AluScalarOpcode;
using xe::gpu::ucode::AluVectorOpcode;

namespace {
    bool CompileOk(const TranslatedShader& shader) {
        if (!shader.ok) {
            std::printf("    traducao falhou: %s\n", shader.error.c_str());
            return false;
        }
        std::vector<uint32_t> spirv;
        std::string log;
        const Stage stage = shader.type == xenos::ShaderType::kVertex ? Stage::kVertex : Stage::kPixel;
        if (!CompileGlslToSpirv(stage, shader.glsl, spirv, log)) {
            std::printf("    glslang: %s\n", log.substr(0, 1500).c_str());
            // Mostra as linhas em volta do primeiro erro ("ERROR: 0:<linha>:").
            const size_t at = log.find("0:");
            if (at != std::string::npos) {
                const int line = std::atoi(log.c_str() + at + 2);
                int current = 1;
                size_t start = 0;
                while (start < shader.glsl.size()) {
                    const size_t end = shader.glsl.find('\n', start);
                    if (current >= line - 3 && current <= line + 1) {
                        std::printf("    %4d| %s\n", current, shader.glsl.substr(start, end - start).c_str());
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                    ++current;
                }
            }
            return false;
        }
        return spirv.size() > 5 && spirv[0] == 0x07230203;
    }

    // VS: r1 = vfetch(fc 0, xyz float); oPos = r1; o0 = c0.
    std::vector<uint32_t> SimpleVertexShader() {
        std::vector<uint32_t> instructions;
        U::EmitVertexFetch(instructions, 1, "xyz1", 0, 0, xenos::VertexFormat::k_32_32_32_FLOAT, 3, 0, true);
        U::EmitAlu(instructions, U::Mov(62, U::R(1), 0xF, true));
        U::EmitAlu(instructions, U::Mov(0, U::C(0), 0xF, true));
        return U::Assemble({ U::Exec(1, 3, 0b001, true), U::Nop() }, instructions);
    }

    // PS: r0 = tex2D(fc 0, r0.xy); oC0 = r0 * c0.
    std::vector<uint32_t> SimplePixelShader() {
        std::vector<uint32_t> instructions;
        U::EmitTextureFetch2D(instructions, 0, "xyzw", 0, 0);
        U::EmitAlu(instructions, U::Vector(AluVectorOpcode::kMul, 0, 0xF, U::R(0), U::C(0), {}, true));
        return U::Assemble({ U::Exec(1, 2, 0b01, true), U::Nop() }, instructions);
    }
}

TEST(translator_simple_vertex_shader_compiles) {
    const std::vector<uint32_t> ucode = SimpleVertexShader();
    const TranslatedShader vs = TranslateShader(ucode.data(), uint32_t(ucode.size()), xenos::ShaderType::kVertex);
    CHECK(CompileOk(vs));
    CHECK_EQ(vs.vertexFetchMask[0], 1);
    CHECK_EQ(vs.interpolatorsWritten, 1);
    CHECK_EQ(vs.instructionCount, 3);
    CHECK_EQ(vs.unsupported, 0);
}

TEST(translator_rectangle_list_vertex_shader_compiles) {
    const std::vector<uint32_t> ucode = SimpleVertexShader();
    const TranslatedShader vs = TranslateShader(ucode.data(), uint32_t(ucode.size()), xenos::ShaderType::kVertex,
                                                VertexShaderMode::kRectangleList);
    CHECK(CompileOk(vs));
}

TEST(translator_simple_pixel_shader_compiles) {
    const std::vector<uint32_t> ucode = SimplePixelShader();
    const TranslatedShader ps = TranslateShader(ucode.data(), uint32_t(ucode.size()), xenos::ShaderType::kPixel);
    CHECK(CompileOk(ps));
    CHECK_EQ(ps.textures.size(), 1);
    CHECK_EQ(ps.colorTargetsWritten, 1);
    CHECK(!ps.killsPixels);
}

TEST(translator_loops_jumps_predicates_and_kill_compile) {
    std::vector<uint32_t> instructions;
    // 0: setp_gt p0 = r0.x > 0 (escalar), 1: kill_eq predicado, 2: export oC0.
    U::Alu setp;
    setp.scalarOp = AluScalarOpcode::kSetpGt;
    setp.vectorMask = 0;
    setp.src[2] = U::R(0, "xxxx");
    U::EmitAlu(instructions, setp);
    U::EmitAlu(instructions, U::Vector(AluVectorOpcode::kKillEq, 1, 0, U::R(0), U::C(1)));
    U::EmitAlu(instructions, U::Mov(0, U::R(0), 0xF, true));
    // cf: 0 loop_start(skip -> 4), 1 exec 1 instr, 2 jmp predicado -> 3, 3 loop_end(body -> 1), 4 exec_end 2 instr
    const std::vector<U::Cf> cfs = { U::LoopStart(4, 0), U::Exec(3, 1, 0, false), U::Jump(3, true, true),
                                     U::LoopEnd(1, 0), U::Exec(4, 2, 0, true), U::Nop() };
    const std::vector<uint32_t> ucode = U::Assemble(cfs, instructions);
    const TranslatedShader ps = TranslateShader(ucode.data(), uint32_t(ucode.size()), xenos::ShaderType::kPixel);
    CHECK(CompileOk(ps));
    CHECK(ps.killsPixels);
}

TEST(translator_rejects_empty_ucode) {
    const TranslatedShader s = TranslateShader(nullptr, 0, xenos::ShaderType::kVertex);
    CHECK(!s.ok);
}

// Todos os shaders extraidos do jogo (tools/extract_shaders.py), se a pasta for informada.
TEST(translator_game_shader_corpus_compiles) {
    const char* dir = std::getenv("GREEN_LANTERN_SHADER_CORPUS");
    if (!dir || !*dir) {
        std::printf("    (GREEN_LANTERN_SHADER_CORPUS nao definido: corpus ignorado)\n");
        return;
    }
    uint32_t total = 0, failed = 0, unsupported = 0;
    std::map<std::string, uint32_t> reasons;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        const bool isVertex = name.find(".vs.") != std::string::npos;
        const bool isPixel = name.find(".ps.") != std::string::npos;
        if (!isVertex && !isPixel) continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> ucode(bytes.size() / 4);
        for (size_t i = 0; i < ucode.size(); ++i) {
            const auto* b = reinterpret_cast<const uint8_t*>(bytes.data() + i * 4);
            ucode[i] = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        }
        ++total;
        const xenos::ShaderType type = isVertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel;
        bool ok = CompileOk(TranslateShader(ucode.data(), uint32_t(ucode.size()), type));
        if (isVertex) {
            ok = ok && CompileOk(TranslateShader(ucode.data(), uint32_t(ucode.size()), type, VertexShaderMode::kRectangleList));
        }
        const TranslatedShader t = TranslateShader(ucode.data(), uint32_t(ucode.size()), type);
        unsupported += t.unsupported;
        if (const char* dump = std::getenv("GREEN_LANTERN_SHADER_DUMP"); dump && *dump) {
            std::ofstream(std::filesystem::path(dump) / (name + ".glsl")) << t.glsl;
        }
        for (size_t at = t.glsl.find("XE_UNSUPPORTED: "); at != std::string::npos;
             at = t.glsl.find("XE_UNSUPPORTED: ", at + 1)) {
            const size_t start = at + 16;
            ++reasons[t.glsl.substr(start, t.glsl.find('\n', start) - start)];
        }
        if (!ok) {
            ++failed;
            std::printf("    falhou: %s\n", name.c_str());
        }
    }
    std::printf("    corpus: %u shaders, %u falharam, %u instrucoes sem suporte\n", total, failed, unsupported);
    for (const auto& [reason, count] : reasons) std::printf("      %s: %u\n", reason.c_str(), count);
    CHECK(total > 0);
    CHECK_EQ(failed, 0);
}
