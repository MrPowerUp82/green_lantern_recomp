#pragma once

#include "types.h"
#include <cstdint>
#include <functional>

namespace Kernel {
    // Executor de funcoes guest para callbacks assincronos (interrupcoes graficas): um unico PPCContext
    // com PCR e stack proprios, criados na primeira chamada. Nao e thread-safe: quem chama serializa
    // (InterruptDispatcher ja faz isso).
    using GuestCallerFn = std::function<void(GuestAddr fn, uint32_t arg0, uint32_t arg1)>;

    GuestCallerFn CreateGuestCaller(uint8_t* base, std::function<GuestAddr(uint32_t)> allocate);
}
