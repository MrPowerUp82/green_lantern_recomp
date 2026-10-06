### Task 2: Import Stubs Generator

**Files:**
- Create: `tools/gen_import_stubs.py`
- Create: `src/kernel/import_stubs.cpp` (generated)

**Interfaces:**
- Consumes: `src/recompiled/ppc_recomp_shared.h`
- Produces: Stubs for `__imp__*` (except `Vd*`) that return 0 and log their names.

- [ ] **Step 1: Write `gen_import_stubs.py`**
Script must parse `src/recompiled/ppc_recomp_shared.h`. Find declarations starting with `__imp__` (e.g. `PPC_EXTERN_FUNC(__imp__...)` or `extern "C" ...`). Ignore any function starting with `__imp__Vd`. Generate C++ code into `src/kernel/import_stubs.cpp`. Each stub should be `extern "C" uint64_t name(uint64_t* context)` and print a simple log to stdout/stderr with its name before returning 0.

- [ ] **Step 2: Run generator and add to build**
Run the script to generate `src/kernel/import_stubs.cpp`. Add `src/kernel/import_stubs.cpp` to the `GpuCore` target in `src/CMakeLists.txt`.

- [ ] **Step 3: Verify build**
Run: `cmake -B build` then `cmake --build build --target GpuCore`
Expected: PASS.
