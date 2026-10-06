# Vulkan GPU Command Processor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a PM4 command processor, `Vd*` exports, and a basic Vulkan Present loop to satisfy the M1 goals for the Green Lantern renderer.

**Architecture:** A dedicated Command Processor (CP) thread decodes PM4 packets from a ring buffer in guest memory, updates the register file, and issues `Swap` commands to a Plume `IGpuBackend`. `Vd*` kernel exports feed this ring buffer and provide setup data from the guest. A stub generator covers the non-`Vd` imports to unblock execution.

**Tech Stack:** C++20, Vulkan (via volk), Plume, CMake, Python (for stubs).

**Spec:** docs/superpowers/specs/2026-10-05-vulkan-gpu-command-processor-design.md

## Global Constraints

- **Pre-existing blockers:** The final executable link step will fail because MSVC lacks GCC/Clang builtins and there are invalid goto labels in `RecompiledCode.lib`. This plan uses an independent `gpu_selftest` executable to validate the M1 milestone.
- **Physical == Virtual:** For memory addressing (e.g., `LOAD_ALU_CONSTANT`), physical addresses map directly to virtual ones in this port. Do not apply physical address masking.
- **Endianness:** The guest (Xbox 360) is big-endian. PM4 packets, struct layouts in `Vd*` queries, and `EVENT_WRITE_SHD` values must be byte-swapped when read/written by the host.

## Review Focus

- **Malformed packets:** A count overflow or out-of-bounds size should discard up to WPTR and not crash the CP.
- **WPTR==RPTR race:** The CP must wait adaptively (spin/sleep) when the ring buffer is empty without burning 100% CPU.
- **Unknown PM4 opcode:** The CP must safely skip unknown opcodes by their packet size without losing ring sync.
- **Vulkan device lost:** `Swap()` failures should not crash the CP thread; it should log and continue consuming the ring.
- **Missing CMake sources:** Ensure `guest_call.cpp` and all new files are added to `CMakeLists.txt` so `gpu_selftest` builds correctly.

---

### Task 1: CMake Setup and `gpu_selftest`

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Create: `src/gpu/gpu_selftest.cpp`

**Interfaces:**
- Produces: `gpu_selftest` executable that links `GpuCore`, `Renderer`, and `volk`, but NOT `RecompiledCode.lib`.

- [ ] **Step 1: Write the basic `gpu_selftest.cpp`**
Create a `main()` that instantiates the Plume renderer, runs a basic window message loop (with ESC to close), and exits cleanly.

- [ ] **Step 2: Update `src/CMakeLists.txt`**
Add `src/kernel/guest_call.cpp` to the `GpuCore` target (it was previously missing).

- [ ] **Step 3: Update `CMakeLists.txt`**
Add the `gpu_selftest` executable target linking `GpuCore`, `Renderer`, and `volk`.

- [ ] **Step 4: Verify build**
Run: `cmake --build build --target gpu_selftest`
Expected: PASS (compiles and links).

### Task 2: Import Stubs Generator

**Files:**
- Create: `tools/gen_import_stubs.py`
- Create: `src/kernel/import_stubs.cpp` (generated)

**Interfaces:**
- Consumes: `src/recompiled/ppc_recomp_shared.h`
- Produces: Stubs for `__imp__*` (except `Vd*`) that return 0 and log their names.

- [ ] **Step 1: Write `gen_import_stubs.py`**
Script must parse `ppc_recomp_shared.h`, ignore any function starting with `__imp__Vd`, and generate C++ code into `src/kernel/import_stubs.cpp`. Each stub should be `extern "C" uint64_t name(uint64_t* context)` and print a log.

- [ ] **Step 2: Run generator and add to build**
Run the script. Add `src/kernel/import_stubs.cpp` to the `GpuCore` target in `src/CMakeLists.txt`.

- [ ] **Step 3: Verify build**
Run: `cmake --build build --target GpuCore`
Expected: PASS.

### Task 3: Base Headers and State

**Files:**
- Create: `src/gpu/gpu_registers.h`
- Create: `src/gpu/gpu_backend.h`
- Create: `src/kernel/gpu_state.h`

**Interfaces:**
- Produces: `IGpuBackend` (`Swap` method), `NullBackend` (for tests), `GpuState` (shared state for `Vd*`).

- [ ] **Step 1: Write `gpu_registers.h`**
Define constants for required registers (e.g., `CP_RB_WPTR`, `CP_RB_RPTR`). Use exact indices derived from the prototype/Xenia reference.

- [ ] **Step 2: Write `gpu_backend.h`**
```cpp
class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;
    virtual void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) = 0;
};
class NullBackend : public IGpuBackend {
public:
    void Swap(uint32_t, uint32_t, uint32_t) override {}
};
```

- [ ] **Step 3: Write `gpu_state.h`**
Define `GpuState` struct containing `uint8_t* base`, `uint32_t wptr_addr`, `uint32_t rptr_writeback_addr`, `uint32_t interrupt_callback`, etc.

- [ ] **Step 4: Commit**
Commit the new headers.

### Task 4: Command Processor - Stage 1 (Ring Buffer)

**Files:**
- Create: `src/gpu/command_processor.h`
- Create: `src/gpu/command_processor.cpp`
- Create: `src/gpu/gpu_tests_stage1.cpp`

**Interfaces:**
- Produces: `CommandProcessor` with a thread loop reading the ring buffer.

- [ ] **Step 1: Write the failing tests in `gpu_tests_stage1.cpp`**
Test type 0 write, type 2 filler, wraparound reading, and RPTR write-back to memory.

- [ ] **Step 2: Run tests to verify failure**
Run: `cmake --build build --target gpu_tests && build/gpu_tests`
Expected: FAIL (missing implementation).

- [ ] **Step 3: Implement `CommandProcessor` ring logic**
Implement the thread loop. Read WPTR. If WPTR == RPTR, adaptative sleep. Decode big-endian dwords. Handle type 0 (registers) and type 2 (filler). Write-back RPTR.

- [ ] **Step 4: Run tests to verify success**
Expected: PASS.

- [ ] **Step 5: Commit**

### Task 5: Command Processor - Stage 2 (Opcodes & Flow)

**Files:**
- Modify: `src/gpu/command_processor.cpp`
- Create: `src/gpu/gpu_tests_stage2.cpp`

**Interfaces:**
- Consumes: `CommandProcessor` decode loop.

- [ ] **Step 1: Write failing tests in `gpu_tests_stage2.cpp`**
Test `WAIT_REG_MEM`, `EVENT_WRITE_SHD` (endianness check), `LOAD_ALU_CONSTANT` (no physical masking), `INDIRECT_BUFFER`, and unknown opcode skipping.

- [ ] **Step 2: Run tests to verify failure**
Expected: FAIL.

- [ ] **Step 3: Implement opcodes in `command_processor.cpp`**
Add type 3 packet decoding. Implement handlers for `WAIT_REG_MEM`, `EVENT_WRITE_SHD`, `REG_RMW`, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, `INDIRECT_BUFFER`, `SET_CONSTANT*`, `LOAD_ALU_CONSTANT`. Skip `DRAW_INDX*` and unknown opcodes.

- [ ] **Step 4: Run tests to verify success**
Expected: PASS.

- [ ] **Step 5: Commit**

### Task 6: Command Processor - Stage 3 (Present & `XE_SWAP`)

**Files:**
- Modify: `src/gpu/command_processor.cpp`
- Create: `src/gpu/gpu_tests_stage3.cpp`

**Interfaces:**
- Consumes: `IGpuBackend::Swap`.

- [ ] **Step 1: Write failing tests in `gpu_tests_stage3.cpp`**
Test `XE_SWAP` opcode issues `Swap` to a mocked backend. Test malformed packets discard up to WPTR safely.

- [ ] **Step 2: Run tests to verify failure**
Expected: FAIL.

- [ ] **Step 3: Implement `XE_SWAP` and malformed packet recovery**
Extract frontbuffer address, width, and height from `XE_SWAP` packet. Call `backend->Swap()`. Add bounds checking for packet counts.

- [ ] **Step 4: Run tests to verify success**
Expected: PASS.

- [ ] **Step 5: Commit**

### Task 7: `Vd*` Exports and Interrupts

**Files:**
- Create: `src/gpu/interrupts.h`, `src/gpu/interrupts.cpp`
- Create: `src/kernel/vd_exports.cpp`
- Create: `src/gpu/gpu_tests_vd.cpp`

**Interfaces:**
- Produces: `__imp__Vd*` implementations for guest calling.

- [ ] **Step 1: Write failing tests in `gpu_tests_vd.cpp`**
Test `VdSwap` generates the correct `XE_SWAP` packet in big-endian. Test `VdQueryVideoMode` returns big-endian structs.

- [ ] **Step 2: Implement interrupts and exports**
Implement a 60Hz thread in `interrupts.cpp` calling `guest_call`. Implement the 20 `Vd*` exports in `vd_exports.cpp` modifying `GpuState` and building `VdSwap` packets.

- [ ] **Step 3: Run tests to verify success**
Expected: PASS.

- [ ] **Step 4: Commit**

### Task 8: Renderer Integration

**Files:**
- Modify: `src/graphics/renderer.h`
- Modify: `src/graphics/renderer.cpp`
- Modify: `src/gpu/gpu_selftest.cpp`

**Interfaces:**
- Consumes: `IGpuBackend`, `CommandProcessor`.

- [ ] **Step 1: Implement `IGpuBackend` in Renderer**
Update the existing Plume renderer to implement `IGpuBackend`. `Swap()` should trigger `acquire`, `clear` (with a distinct color to prove it works), and `present`.

- [ ] **Step 2: Update `gpu_selftest.cpp`**
Wire the `CommandProcessor` to the Plume renderer instance. Feed it a synthetic ring buffer that issues an `XE_SWAP` packet.

- [ ] **Step 3: Execute `gpu_selftest`**
Run: `build/gpu_selftest`
Expected: A window opens, clears to the specified color, logs the swap, and exits cleanly.

- [ ] **Step 4: Commit**
