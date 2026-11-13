cmake_minimum_required(VERSION 3.25)
project(GreenLanternRecompiledCode LANGUAGES C CXX)

file(GLOB RECOMP_SOURCES "ppc_recomp.*.cpp" "ppc_func_mapping.cpp")

add_library(RecompiledCode STATIC ${RECOMP_SOURCES})

target_include_directories(RecompiledCode PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
    ${CMAKE_SOURCE_DIR}/include
)

if (MSVC)
    target_compile_options(RecompiledCode PRIVATE /MP /arch:AVX2 /O2 /bigobj /W0)
    target_compile_definitions(RecompiledCode PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(RecompiledCode PRIVATE -mavx2 -mfma -O3 -w)
endif()

# Estas unidades tem funcoes de milhares de linhas com muitos goto; o otimizador do MSVC leva horas
# nelas com /O2. Sem otimizacao compilam em segundos (o codigo gerado e o mesmo, so mais lento).
include(${CMAKE_CURRENT_SOURCE_DIR}/slow_units.cmake OPTIONAL)
if (MSVC AND PPC_SLOW_UNITS)
    set_source_files_properties(${PPC_SLOW_UNITS} PROPERTIES COMPILE_OPTIONS "/Od")
endif()
