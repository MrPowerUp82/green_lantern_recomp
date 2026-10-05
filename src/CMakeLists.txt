# Core Engine Library (Kernel, Platform, GPU)
add_library(GpuCore STATIC
    platform/filesystem.cpp
    kernel/memory.cpp
    kernel/kernel_stubs.cpp
    kernel/guest_call.cpp
)

target_include_directories(GpuCore PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/XenonUtils
)

# Graphics Renderer Library (Plume Vulkan / Direct3D 12)
add_library(Renderer STATIC
    graphics/renderer.cpp
)

target_include_directories(Renderer PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/thirdparty/plume/contrib/volk
    ${CMAKE_SOURCE_DIR}/thirdparty/plume/contrib/Vulkan-Headers/include
)

target_link_libraries(Renderer PUBLIC plume volk)
if (WIN32)
    target_link_libraries(Renderer PUBLIC d3d12 dxgi dxguid)
endif()
