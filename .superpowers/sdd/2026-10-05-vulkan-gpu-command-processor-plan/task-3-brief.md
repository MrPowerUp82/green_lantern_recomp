### Task 3: Base Headers and State

**Files:**
- Create: `src/gpu/gpu_registers.h`
- Create: `src/gpu/gpu_backend.h`
- Create: `src/kernel/gpu_state.h`

**Interfaces:**
- Produces: `IGpuBackend` (`Swap` method), `NullBackend` (for tests), `GpuState` (shared state for `Vd*`).

- [ ] **Step 1: Write `gpu_registers.h`**
Define constants for required registers. According to Xenia/PM4 specs, define at least: `CP_RB_WPTR` (e.g., `0x1C` in the register page or similar offset). Define any other basic registers needed for PM4. Put them in `namespace gpu { namespace reg { ... } }`.

- [ ] **Step 2: Write `gpu_backend.h`**
```cpp
#pragma once
#include <cstdint>
namespace gpu {
class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;
    virtual void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) = 0;
};
class NullBackend : public IGpuBackend {
public:
    void Swap(uint32_t, uint32_t, uint32_t) override {}
};
}
```

- [ ] **Step 3: Write `gpu_state.h`**
```cpp
#pragma once
#include <cstdint>
namespace kernel {
struct GpuState {
    uint8_t* base = nullptr;
    uint32_t wptr_addr = 0;
    uint32_t rptr_writeback_addr = 0;
    uint32_t interrupt_callback = 0;
    uint32_t interrupt_callback_user_data = 0;
};
}
```

- [ ] **Step 4: Commit**
Commit the new headers. Ensure they are added to `GpuCore` target in `src/CMakeLists.txt`. Verify build.
