// Imports de memoria do xboxkrnl.exe: Nt*VirtualMemory, Mm* e pools do kernel.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>

using namespace Kernel;

namespace {
    constexpr uint32_t kMemCommit = 0x1000;
    constexpr uint32_t kMemReserve = 0x2000;
    constexpr uint32_t kMemDecommit = 0x4000;
    constexpr uint32_t kMemRelease = 0x8000;
    constexpr uint32_t kPageReadWrite = 0x04;
    constexpr uint32_t kPageSize = 0x1000;

    uint32_t Ld32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }

    // Pool pequeno: blocos < 64 KB saem de chunks de 1 MB, sem liberacao (o kernel real tambem os
    // mantem em pools); blocos grandes vao direto para o heap do guest.
    struct PoolState {
        std::mutex lock;
        GuestAddr chunk = 0;
        uint32_t used = 0;
        uint32_t size = 0;
    };

}

namespace Kernel {
    GuestAddr PoolAlloc(uint32_t size) {
        static PoolState pool;
        if (size == 0) size = 1;
        if (size >= 0x10000) return MemoryManager::AllocateAligned(size, kPageSize, false);
        std::lock_guard<std::mutex> lock(pool.lock);
        const uint32_t rounded = (size + 15) & ~15u;
        if (!pool.chunk || pool.used + rounded > pool.size) {
            pool.size = 0x100000;
            pool.chunk = MemoryManager::AllocateAligned(pool.size, kPageSize, false);
            pool.used = 0;
            if (!pool.chunk) return 0;
        }
        const GuestAddr result = pool.chunk + pool.used;
        pool.used += rounded;
        return result;
    }
}

namespace {
}

// r3 = base*, r4 = tamanho*, r5 = tipo de alocacao, r6 = protecao
KERNEL_FUNC(__imp__NtAllocateVirtualMemory) {
    const uint32_t basePtr = ctx.r3.u32;
    const uint32_t sizePtr = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32;
    if (!basePtr || !sizePtr) { ctx.r3.u64 = Status::kInvalidParameter; return; }

    GuestAddr address = Ld32(base, basePtr);
    uint32_t size = Ld32(base, sizePtr);
    if (size == 0) { ctx.r3.u64 = Status::kInvalidParameter; return; }
    // MEM_LARGE_PAGES (0x20000000): paginas de 64 KB. O kernel arredonda base e tamanho para elas e
    // devolve o tamanho arredondado; o heap do jogo conta com isso para fechar a lista de faixas
    // nao commitadas.
    const uint32_t pageSize = (type & 0x20000000u) ? 0x10000u : kPageSize;
    if (address != 0) address &= ~(pageSize - 1);
    size = (size + pageSize - 1) & ~(pageSize - 1);

    if (address != 0) {
        // Commit sobre uma regiao ja reservada, ou reserva de um endereco fixo.
        if (MemoryManager::AllocationSize(address) < size) {
            const GuestAddr fixed = MemoryManager::AllocateAligned(size, kPageSize, false, address);
            // Fora do heap gerenciado (imagem, stacks pre-commitadas): aceita sem alocar nada.
            (void)fixed;
        }
        St32(base, basePtr, address);
        St32(base, sizePtr, size);
        if (g_traceKernel) std::fprintf(stderr, "[trace]   alloc fixo base=%08X tamanho=%08X (bloco %08X)\n", address, size, MemoryManager::AllocationSize(address));
        ctx.r3.u64 = Status::kSuccess;
        return;
    }

    address = MemoryManager::AllocateAligned(size, 0x10000, false);
    if (!address) {
        std::cerr << "[Kernel] NtAllocateVirtualMemory sem memoria (tamanho 0x" << std::hex << size << std::dec << ")" << std::endl;
        ctx.r3.u64 = Status::kNoMemory;
        return;
    }
    (void)type;
    St32(base, basePtr, address);
    St32(base, sizePtr, size);
    if (g_traceKernel) std::fprintf(stderr, "[trace]   alloc novo base=%08X tamanho=%08X\n", address, size);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = base*, r4 = tamanho*, r5 = tipo de liberacao
KERNEL_FUNC(__imp__NtFreeVirtualMemory) {
    const GuestAddr address = ctx.r3.u32 ? Ld32(base, ctx.r3.u32) : 0;
    if (ctx.r5.u32 & kMemRelease) {
        const GuestSize size = MemoryManager::AllocationSize(address);
        MemoryManager::FreeVirtual(address);
        if (ctx.r4.u32) St32(base, ctx.r4.u32, size);
    }
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = endereco, r4 = MEMORY_BASIC_INFORMATION*
KERNEL_FUNC(__imp__NtQueryVirtualMemory) {
    const GuestAddr address = ctx.r3.u32;
    const uint32_t info = ctx.r4.u32;
    if (!info) { ctx.r3.u64 = Status::kInvalidParameter; return; }
    uint32_t size = MemoryManager::AllocationSize(address);
    if (size == 0) size = kPageSize;  // regioes pre-commitadas fora do heap
    const GuestAddr pageBase = address & ~(kPageSize - 1);
    St32(base, info + 0x00, pageBase);          // base_address
    St32(base, info + 0x04, pageBase);          // allocation_base
    St32(base, info + 0x08, kPageReadWrite);    // allocation_protect
    St32(base, info + 0x0C, size);              // region_size
    St32(base, info + 0x10, kMemCommit);        // state
    St32(base, info + 0x14, kPageReadWrite);    // protect
    St32(base, info + 0x18, 0x20000);           // type: MEM_PRIVATE
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = flags, r4 = tamanho, r5 = protecao, r6 = minimo, r7 = maximo, r8 = alinhamento -> endereco (ou 0)
KERNEL_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    uint32_t alignment = ctx.r8.u32 ? ctx.r8.u32 : kPageSize;
    if (alignment < kPageSize) alignment = kPageSize;
    ctx.r3.u64 = MemoryManager::AllocateAligned(ctx.r4.u32, alignment, true);
}

// r3 = tipo, r4 = base
KERNEL_FUNC(__imp__MmFreePhysicalMemory) {
    MemoryManager::FreeVirtual(ctx.r4.u32);
}

KERNEL_FUNC(__imp__MmQueryAllocationSize) { ctx.r3.u64 = MemoryManager::AllocationSize(ctx.r3.u32); }
KERNEL_FUNC(__imp__MmQueryAddressProtect) { ctx.r3.u64 = kPageReadWrite; }
KERNEL_FUNC(__imp__MmSetAddressProtect) { (void)ctx; }
KERNEL_FUNC(__imp__MmMapIoSpace) { ctx.r3.u64 = ctx.r3.u32; }

// r3 = X_MM_QUERY_STATISTICS_RESULT* (paginas de 4 KB; 512 MB de RAM)
KERNEL_FUNC(__imp__MmQueryStatistics) {
    const uint32_t ptr = ctx.r3.u32;
    if (!ptr) { ctx.r3.u64 = Status::kInvalidParameter; return; }
    std::memset(base + ptr, 0, 0x68);
    St32(base, ptr + 0x00, 0x68);       // tamanho da estrutura
    St32(base, ptr + 0x04, 0x20000);    // paginas totais
    St32(base, ptr + 0x08, 0x300);      // paginas do kernel
    St32(base, ptr + 0x0C, 0x1E000);    // paginas livres do titulo
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = tamanho, r4 = tag, r5 = tipo de pool
KERNEL_FUNC(__imp__ExAllocatePoolTypeWithTag) { ctx.r3.u64 = PoolAlloc(ctx.r3.u32); }
KERNEL_FUNC(__imp__ExAllocatePoolWithTag) { ctx.r3.u64 = PoolAlloc(ctx.r3.u32); }
KERNEL_FUNC(__imp__ExFreePool) { (void)ctx; }  // pool sem liberacao (ver PoolAlloc)

// r3 = flags, r4 = tamanho, r5 = ponteiro de saida
KERNEL_FUNC(__imp__XamAlloc) {
    const GuestAddr ptr = PoolAlloc(ctx.r4.u32);
    if (ctx.r5.u32) St32(base, ctx.r5.u32, ptr);
    ctx.r3.u64 = ptr ? Status::kSuccess : Status::kNoMemory;
}
