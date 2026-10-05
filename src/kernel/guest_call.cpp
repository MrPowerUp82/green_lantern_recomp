#include "guest_call.h"
#include <iostream>

namespace Kernel {
    void GuestCall(GuestAddr functionAddr, uint64_t arg0, uint64_t arg1) {
        (void)arg0;
        (void)arg1;
        if (functionAddr == 0) return;
        std::cout << "[Kernel] GuestCall chamado para endereco 0x" << std::hex << functionAddr << std::dec << "\n";
    }
}
