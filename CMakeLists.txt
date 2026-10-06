cmake_minimum_required(VERSION 3.25)
project(GreenLanternRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compilador e Flags de Otimização (AVX2 é essencial para emulação dos registradores VMX128 do Xenon)
if (MSVC)
    add_compile_options(/W3 /MP /arch:AVX2 /permissive- /Zc:preprocessor)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -mavx2 -mfma)
endif()

# O PPCContext inclui x86/avx.h a partir da pasta interna do SIMDe.
find_path(SIMDE_INCLUDE_DIR x86/avx.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde/simde
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/simde/simde
)
if (NOT SIMDE_INCLUDE_DIR)
    message(FATAL_ERROR "SIMDe ausente: inicialize XenonRecomp ou informe -DSIMDE_INCLUDE_DIR=<pasta que contem x86/avx.h>")
endif()

# Diretórios de include
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SIMDE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
)

# Permite testar PM4 sem Plume, assets ou unidades recompiladas do jogo.
option(GREEN_LANTERN_BUILD_GAME "Compila o executavel com o codigo recompilado do jogo" ON)
option(GREEN_LANTERN_GPU_CORE_ONLY "Compila apenas GpuCore e gpu_tests" OFF)
find_package(Threads REQUIRED)

# Núcleo isolado: só headers PPC, sem MemoryManager nem tabela de despacho.
add_subdirectory(src)
enable_testing()
file(GLOB GPU_TEST_SOURCES CONFIGURE_DEPENDS tests/gpu/*.cpp)
add_executable(gpu_tests ${GPU_TEST_SOURCES})
target_include_directories(gpu_tests PRIVATE tests/gpu)
target_link_libraries(gpu_tests PRIVATE GpuCore)
add_test(NAME gpu_tests COMMAND gpu_tests)
set_tests_properties(gpu_tests PROPERTIES TIMEOUT 30)
if (GREEN_LANTERN_GPU_CORE_ONLY)
    return()
endif()

# Adicionar submódulos e código recompilado
if (GREEN_LANTERN_BUILD_GAME)
    add_subdirectory(src/recompiled)
endif()

# Camada de abstração gráfica (Vulkan / D3D12)
add_subdirectory(thirdparty/plume)

# O plume usa o volk como loader do Vulkan mas não compila o volk.c
add_library(volk STATIC thirdparty/plume/contrib/volk/volk.c)
target_include_directories(volk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/contrib/volk
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume/contrib/Vulkan-Headers/include
)
if (WIN32)
    target_compile_definitions(volk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
target_link_libraries(plume PUBLIC volk)

# Renderer depende de Plume; GpuCore continua independente.
add_library(Renderer STATIC src/graphics/renderer.cpp)
target_link_libraries(Renderer PUBLIC GpuCore plume volk)
if (WIN32)
    target_link_libraries(Renderer PUBLIC d3d12 dxgi dxguid)
endif()

if (GREEN_LANTERN_BUILD_GAME)
# Definição do Executável Principal
set(SOURCES
    src/main.cpp
    src/kernel/guest_thread.cpp
    src/kernel/guest_call.cpp
    src/kernel/memory.cpp
    src/kernel/kernel_stubs.cpp
    src/kernel/import_stubs.cpp
    src/platform/filesystem.cpp
)

add_executable(GreenLanternRecomp ${SOURCES})

target_link_libraries(GreenLanternRecomp PRIVATE GpuCore Renderer RecompiledCode plume volk)
if (WIN32)
    target_link_libraries(GreenLanternRecomp PRIVATE d3d12 dxgi dxguid)
endif()

# Configuração de Output
set_target_properties(GreenLanternRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

endif()

# Alvo de Auto-teste da GPU (M1) - isolado de RecompiledCode.lib
add_executable(gpu_selftest src/gpu_selftest/main.cpp)

target_link_libraries(gpu_selftest PRIVATE GpuCore Renderer volk)
if (WIN32)
    target_link_libraries(gpu_selftest PRIVATE d3d12 dxgi dxguid)
endif()

set_target_properties(gpu_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
