# O núcleo não referencia Plume, MemoryManager ou símbolos do jogo recompilado.
add_library(GpuCore STATIC
    gpu/command_processor.cpp
    gpu/interrupts.cpp
    gpu/gpu_system.cpp
    gpu/selftest.cpp
    kernel/vd_exports.cpp
    kernel/mm_exports.cpp
)
target_include_directories(GpuCore PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(GpuCore PUBLIC Threads::Threads)
