cmake_minimum_required(VERSION 3.25)
project(DumpXexImage LANGUAGES C CXX)

# Usa as bibliotecas ja compiladas do XenonRecomp (tools/XenonRecomp/build, config Release).
set(XENON_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp)
set(XENON_BUILD ${XENON_ROOT}/build)

set(CMAKE_CXX_STANDARD 20)

add_executable(dump_xex_image dump_xex_image.cpp)
target_include_directories(dump_xex_image PRIVATE
    ${XENON_ROOT}/XenonUtils
    ${XENON_ROOT}/thirdparty/simde)
target_link_libraries(dump_xex_image PRIVATE
    ${XENON_BUILD}/XenonUtils/Release/XenonUtils.lib
    ${XENON_BUILD}/thirdparty/disasm/Release/disasm.lib)
target_compile_definitions(dump_xex_image PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_target_properties(dump_xex_image PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
