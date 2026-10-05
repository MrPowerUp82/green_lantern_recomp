#pragma once

#include "types.h"

namespace Kernel {
    // Invoca um callback ou função do guest (Xenon) a partir do host.
    void GuestCall(GuestAddr functionAddr, uint64_t arg0 = 0, uint64_t arg1 = 0);
}
