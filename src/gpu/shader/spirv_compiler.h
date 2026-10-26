#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Gpu::Shader {
    enum class Stage { kVertex, kPixel };

    // GLSL 4.50 -> SPIR-V 1.0 (Vulkan 1.0) via glslang. Thread-safe; o processo do glslang e
    // inicializado na primeira chamada. Em caso de erro, `log` recebe a mensagem do compilador.
    bool CompileGlslToSpirv(Stage stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& log);
}
