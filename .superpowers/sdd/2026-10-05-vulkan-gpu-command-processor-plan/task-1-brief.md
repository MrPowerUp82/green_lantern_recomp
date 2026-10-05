### Task 1: CMake Setup and `gpu_selftest`

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Create: `src/gpu/gpu_selftest.cpp`

**Interfaces:**
- Produces: `gpu_selftest` executable that links `GpuCore`, `Renderer`, and `volk`, but NOT `RecompiledCode.lib`.

- [ ] **Step 1: Write the basic `gpu_selftest.cpp`**
Create a `main()` that instantiates the Plume renderer (from `src/graphics/renderer.h`), runs a basic window message loop (with ESC to close), and exits cleanly. (Note: use Win32 message loop or similar as appropriate for the existing PlumeRenderer).

- [ ] **Step 2: Update `src/CMakeLists.txt`**
Add `src/kernel/guest_call.cpp` to the `GpuCore` target (it was previously missing).

- [ ] **Step 3: Update `CMakeLists.txt`**
Add the `gpu_selftest` executable target linking `GpuCore`, `Renderer`, and `volk`.

- [ ] **Step 4: Verify build**
Run: `cmake -B build` then `cmake --build build --target gpu_selftest`
Expected: PASS (compiles and links).
