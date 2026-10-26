#include "gpu/shader/spirv_compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>

namespace Gpu::Shader {
    namespace {
        void EnsureProcessInitialized() {
            static std::once_flag once;
            std::call_once(once, [] { glslang::InitializeProcess(); });
        }
    }

    bool CompileGlslToSpirv(Stage stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& log) {
        EnsureProcessInitialized();
        const EShLanguage language = stage == Stage::kVertex ? EShLangVertex : EShLangFragment;

        glslang::TShader shader(language);
        const char* source = glsl.c_str();
        shader.setStrings(&source, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
        shader.setEntryPoint("main");

        const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
            log = shader.getInfoLog();
            return false;
        }

        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            log = program.getInfoLog();
            return false;
        }

        glslang::SpvOptions options;
        options.disableOptimizer = true;
        spirv.clear();
        glslang::GlslangToSpv(*program.getIntermediate(language), spirv, &options);
        return !spirv.empty();
    }
}
