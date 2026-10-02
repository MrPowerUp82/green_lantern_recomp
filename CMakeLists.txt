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

# Definição do Executável Principal
set(SOURCES
    src/main.cpp
    src/platform/filesystem.cpp
    src/kernel/memory.cpp
    src/kernel/kernel_stubs.cpp
    src/graphics/renderer.cpp
)

add_executable(GreenLanternRecomp ${SOURCES})

target_link_libraries(GreenLanternRecomp PRIVATE RecompiledCode)

# Configuração de Output
set_target_properties(GreenLanternRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
