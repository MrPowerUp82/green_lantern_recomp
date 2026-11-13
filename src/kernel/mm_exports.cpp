// Imports Mm* do xboxkrnl.exe com implementacao real.

#include "recompiled/ppc_context.h"

// r3 = endereco virtual -> r3 = endereco fisico. Este port nao separa fisico de virtual: o ring
// buffer e os buffers que o command processor le ficam no mesmo espaco de enderecos do guest,
// entao a identidade basta.
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    // A memoria "fisica" do kernel (0xA0000000-0xBFFFFFFF) espelha a RAM fisica 0-0x1FFFFFFF; a GPU
    // recebe o endereco fisico e GuestMemory (physicalAlias) o devolve a faixa 0xA0000000+.
    const uint32_t address = ctx.r3.u32;
    ctx.r3.u64 = (address >= 0xA0000000u && address < 0xC0000000u) ? (address & 0x1FFFFFFFu) : address;
}
