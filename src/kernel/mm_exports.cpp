// Imports Mm* do xboxkrnl.exe com implementacao real.

#include "recompiled/ppc_context.h"

// r3 = endereco virtual -> r3 = endereco fisico. Este port nao separa fisico de virtual: o ring
// buffer e os buffers que o command processor le ficam no mesmo espaco de enderecos do guest,
// entao a identidade basta.
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    ctx.r3.u64 = ctx.r3.u32;
}
