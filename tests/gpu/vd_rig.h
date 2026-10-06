#pragma once

#include "recompiled/ppc_context.h"

#include "gpu/gpu_system.h"
#include "cp_rig.h"

#include <mutex>
#include <vector>

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSetGraphicsInterruptCallback);
PPC_EXTERN_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress);
PPC_EXTERN_FUNC(__imp__VdInitializeEngines);
PPC_EXTERN_FUNC(__imp__VdShutdownEngines);
PPC_EXTERN_FUNC(__imp__VdRetrainEDRAM);
PPC_EXTERN_FUNC(__imp__VdRetrainEDRAMWorker);
PPC_EXTERN_FUNC(__imp__VdEnableDisableClockGating);
PPC_EXTERN_FUNC(__imp__VdIsHSIOTrainingSucceeded);
PPC_EXTERN_FUNC(__imp__VdCallGraphicsNotificationRoutines);
PPC_EXTERN_FUNC(__imp__VdQueryVideoMode);
PPC_EXTERN_FUNC(__imp__VdQueryVideoFlags);
PPC_EXTERN_FUNC(__imp__VdSetDisplayMode);
PPC_EXTERN_FUNC(__imp__VdGetCurrentDisplayInformation);
PPC_EXTERN_FUNC(__imp__VdGetCurrentDisplayGamma);
PPC_EXTERN_FUNC(__imp__VdPersistDisplay);
PPC_EXTERN_FUNC(__imp__VdGetSystemCommandBuffer);
PPC_EXTERN_FUNC(__imp__VdInitializeScalerCommandBuffer);
PPC_EXTERN_FUNC(__imp__VdSwap);

using namespace Gpu;
using namespace TestSupport;

namespace {
    // GpuSystem sobre a memoria de teste, com backend nulo e um "guest" que so registra chamadas.
    struct VdRig {
        explicit VdRig(bool interrupts = false) : mem(GuestBase()) {
            GpuSystemConfig config;
            config.base = GuestBase();
            config.backend = &backend;
            config.allocate = [](uint32_t size) { return Allocate(size); };
            config.caller = [this](GuestAddr fn, uint32_t a0, uint32_t a1) {
                std::lock_guard<std::mutex> lock(callMutex);
                calls.push_back({ fn, a0, a1 });
            };
            config.enableInterrupts = interrupts;
            GpuSystem::Get().Initialize(config);
            ctx.r1.u64 = Allocate(0x1000) + 0x800;  // pilha fake com espaco para os argumentos de pilha
        }
        ~VdRig() { GpuSystem::Get().Shutdown(); }

        struct Call { GuestAddr fn; uint32_t a0; uint32_t a1; };

        NullBackend backend;
        GuestMemory mem;
        PPCContext ctx{};
        std::mutex callMutex;
        std::vector<Call> calls;
    };
}
