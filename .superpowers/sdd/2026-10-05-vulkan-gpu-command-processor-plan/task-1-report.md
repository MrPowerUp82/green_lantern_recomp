# Task 1 Report: CMake Setup and `gpu_selftest`

## Implementation Summary
- Created `src/kernel/guest_call.h` and `src/kernel/guest_call.cpp` implementing host-to-guest callback invocation stub.
- Created `src/gpu/gpu_selftest.cpp` implementing a standalone test executable that initializes `Graphics::NativeRenderer` (Plume Vulkan backend with D3D12 fallback), runs a frame loop polling events with ESC key / window close support, and cleanly shuts down. Added `--frames <N>` and `--timeout-ms <N>` CLI parameters to facilitate automated/headless testing.
- Created `src/CMakeLists.txt` defining modular library targets:
  - `GpuCore` (STATIC): `platform/filesystem.cpp`, `kernel/memory.cpp`, `kernel/kernel_stubs.cpp`, `kernel/guest_call.cpp`.
  - `Renderer` (STATIC): `graphics/renderer.cpp`, linking `plume`, `volk`, and system DXGI/D3D12 libraries.
- Modified root `CMakeLists.txt`:
  - Added `add_subdirectory(src)`
  - Updated `GreenLanternRecomp` to link `GpuCore` and `Renderer`
  - Added `gpu_selftest` executable linking `GpuCore`, `Renderer`, and `volk` (and Direct3D/DXGI on Win32) without any dependency on `RecompiledCode.lib`.

## Verification & Test Results
- **CMake configure:** `cmake -B build` exited 0.
- **Build GpuCore target:** `cmake --build build --target GpuCore` exited 0 (`GpuCore.lib` built cleanly).
- **Build Renderer target:** `cmake --build build --target Renderer` exited 0 (`Renderer.lib` built cleanly).
- **Build gpu_selftest target:** `cmake --build build --target gpu_selftest` exited 0 (`gpu_selftest.exe` built cleanly).
- **Runtime test:** `.\build\bin\Debug\gpu_selftest.exe --frames 5` executed successfully:
  - Vulkan renderer initialized on Intel(R) UHD Graphics.
  - Rendered 5 frames cleanly.
  - Successfully shut down and exited with code 0.

## Files Changed
- `CMakeLists.txt` (modified)
- `src/CMakeLists.txt` (created)
- `src/gpu/gpu_selftest.cpp` (created)
- `src/kernel/guest_call.h` (created)
- `src/kernel/guest_call.cpp` (created)

## Self-Review Findings
- **Completeness:** All 4 steps of Task 1 implemented and verified.
- **Independence:** `gpu_selftest` does not link `RecompiledCode.lib`, completely avoiding the pre-existing link blockers in the recompiled code.
- **Discipline:** No unnecessary code or dependencies added; followed the project's C++20 conventions.

## Concerns
- In a git worktree, untracked dependencies (`thirdparty/plume`, `tools/XenonRecomp`, `tools/XenosRecomp`) and gitignored recompiled source files (`src/recompiled/*.cpp`) needed junction links / local copies from the parent repo to enable CMake configuration of all subdirectories. These are properly set up in the worktree now.
