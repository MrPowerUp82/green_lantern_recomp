# Formatos e microcodigo do Xenos portados do Xenia (licenca BSD em gpu/xenos/LICENSE.xenia).
add_library(XenosFormats STATIC
    gpu/xenos/xenia/gpu/xenos.cc
    gpu/xenos/xenia/gpu/ucode.cc
    gpu/xenos/xenia/gpu/registers.cc
    gpu/xenos/xenia/gpu/texture_info.cc
    gpu/xenos/xenia/gpu/texture_info_formats.cc
    gpu/xenos/xenia/gpu/texture_util.cc
)
target_include_directories(XenosFormats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gpu/xenos)
if (MSVC)
    target_compile_options(XenosFormats PRIVATE /W1)
endif()

# O núcleo não referencia Plume, MemoryManager ou símbolos do jogo recompilado.
add_library(GpuCore STATIC
    gpu/command_processor.cpp
    gpu/guest_memory.cpp
    gpu/interrupts.cpp
    gpu/gpu_system.cpp
    gpu/selftest.cpp
    gpu/selftest_scene.cpp
    kernel/vd_exports.cpp
    kernel/mm_exports.cpp
)
target_include_directories(GpuCore PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(GpuCore PUBLIC Threads::Threads XenosFormats)

# Tradutor de microcodigo Xenos -> GLSL (sem dependencias de GPU).
add_library(XenosShader STATIC gpu/shader/glsl_translator.cpp)
target_include_directories(XenosShader PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(XenosShader PUBLIC XenosFormats)

# GLSL -> SPIR-V (glslang), usado pelo renderer e pelos testes do tradutor.
if (TARGET glslang)
    add_library(ShaderCompiler STATIC gpu/shader/spirv_compiler.cpp)
    target_include_directories(ShaderCompiler PUBLIC ${CMAKE_SOURCE_DIR}/src
        PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/glslang)
    target_link_libraries(ShaderCompiler PUBLIC glslang SPIRV glslang-default-resource-limits)
endif()

# Preparacao de draws na CPU (indices, estado de render, resolve, texturas), sem API grafica.
add_library(XenosDraw STATIC
    gpu/draw/primitive_processor.cpp
    gpu/draw/draw_state.cpp
    gpu/draw/resolve_info.cpp
    gpu/draw/texture_loader.cpp
)
target_link_libraries(XenosDraw PUBLIC GpuCore XenosShader)
