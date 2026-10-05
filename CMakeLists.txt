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

# Diretórios de include
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
)

# Adicionar submódulos e código recompilado
add_subdirectory(src/recompiled)

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

# Subsistemas do engine (GpuCore, Renderer)
add_subdirectory(src)

# Definição do Executável Principal
set(SOURCES
    src/main.cpp
    src/kernel/guest_thread.cpp
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

# Alvo de Auto-teste da GPU (M1) - isolado de RecompiledCode.lib
add_executable(gpu_selftest src/gpu/gpu_selftest.cpp)

target_link_libraries(gpu_selftest PRIVATE GpuCore Renderer volk)
if (WIN32)
    target_link_libraries(gpu_selftest PRIVATE d3d12 dxgi dxguid)
endif()

set_target_properties(gpu_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
