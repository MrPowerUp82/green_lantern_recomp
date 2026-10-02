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
