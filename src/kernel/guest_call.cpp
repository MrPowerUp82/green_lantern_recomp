#include "guest_call.h"

#include "../recompiled/ppc_context.h"
#include "guest_thread.h"

#include <cstring>
#include <iostream>
#include <memory>

namespace Kernel {
    namespace {
        constexpr uint32_t kCallbackStackSize = 0x40000;  // 256 KB

        struct CallerState {
            GuestAddr stack = 0;
            GuestAddr pcr = 0;
            bool ready = false;
            bool loggedBadAddress = false;
        };
    }

    GuestCallerFn CreateGuestCaller(uint8_t* base, std::function<GuestAddr(uint32_t)> allocate) {
        auto state = std::make_shared<CallerState>();

        return [base, allocate = std::move(allocate), state](GuestAddr fn, uint32_t arg0, uint32_t arg1) {
            if (fn < PPC_CODE_BASE || fn >= PPC_CODE_BASE + PPC_CODE_SIZE) {
                if (!state->loggedBadAddress) {
                    state->loggedBadAddress = true;
                    std::cerr << "[Guest] callback fora do codigo recompilado: 0x" << std::hex << fn << std::dec << std::endl;
                }
                return;
            }
            PPCFunc* host = PPC_LOOKUP_FUNC(base, fn);
            if (!host) {
                if (!state->loggedBadAddress) {
                    state->loggedBadAddress = true;
                    std::cerr << "[Guest] callback sem funcao recompilada: 0x" << std::hex << fn << std::dec << std::endl;
                }
                return;
            }

            if (!state->ready) {
                state->stack = allocate(kCallbackStackSize);
                state->pcr = allocate(GuestThread::kPcrSize);
                if (!state->stack || !state->pcr) return;
                std::memset(base + state->pcr, 0, GuestThread::kPcrSize);
                // Mesmos campos do PCR da thread principal (guest_thread.cpp): topo e base da stack.
                Endian::WriteBe32(base + state->pcr + 0x70, state->stack + kCallbackStackSize);
                Endian::WriteBe32(base + state->pcr + 0x74, state->stack);
                state->ready = true;
            }

            PPCContext ctx{};
            ctx.fpscr.loadFromHost();
            ctx.r1.u64 = state->stack + kCallbackStackSize - 0x100;
            ctx.r13.u64 = state->pcr;
            ctx.r3.u64 = arg0;
            ctx.r4.u64 = arg1;
            ctx.lr = 0;
            host(ctx, base);
        };
    }
}
