# Command Processor PM4 + Present (Renderer Vulkan, sub-projeto 1) — Plano de Implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fazer o jogo conversar com uma GPU emulada: imports `Vd*`, command processor PM4 sobre o ring buffer, interrupções gráficas e present de cada `XE_SWAP` na swapchain Plume (Vulkan).

**Architecture:** O D3D do 360 escreve pacotes PM4 num ring buffer e publica o write-pointer numa página MMIO (`0x7FC80000`, registrador `0x01C5`, endereço `0x7FC80714`). Uma thread do CP faz polling desse WPTR, decodifica os pacotes e chama um `IGpuBackend` (o renderer Plume). Os `Vd*` são `PPC_FUNC` livres que acessam um `GpuSystem` global. Tudo que não depende de Plume nem do código recompilado fica na biblioteca `GpuCore`, testada por `gpu_tests`.

**Tech Stack:** C++20, MSVC (Visual Studio 18 / `cl`), CMake 3.25+, Plume (Vulkan/D3D12), `ppc_context.h` do XenonRecomp, Python 3 (gerador de stubs). Sem framework de teste: harness mínimo próprio.

**Spec:** `docs/superpowers/specs/2026-10-05-vulkan-gpu-command-processor-design.md` (com as emendas de 2026-10-06).

## Global Constraints

- Idioma: comentários e mensagens de log em português, sem acentos nos logs (padrão do projeto), como nos arquivos existentes.
- Plataforma: Windows x64, MSVC; flags globais do `CMakeLists.txt` raiz (`/W3 /MP /arch:AVX2 /permissive- /Zc:preprocessor`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`). C++20.
- A memória do guest é big-endian; os registradores/PM4 lidos pelo CP são decodificados com `GuestMemory::LoadBe32`.
- Ring: `(1 << (size_log2 + 3))` bytes. WPTR/RPTR são índices em dwords. WPTR é lido da página MMIO em `Reg::MmioAddress(Reg::kCpRbWptr)` (big-endian).
- `CommandProcessor` recebe `base` por parâmetro e nunca usa o `MemoryManager`; o `GpuCore` não pode depender de Plume nem de `RecompiledCode`.
- Físico == virtual neste port (`MmGetPhysicalAddress` é a identidade).
- Pacote malformado: loga uma vez, descarta até o WPTR, incrementa `malformed`; o CP nunca trava o jogo. Opcode desconhecido: pula pelo tamanho e conta.
- Commits: nunca `git add -A` (o repo tem `thirdparty/` e `tools/XenonRecomp|XenosRecomp` não rastreados). Adicionar só os arquivos listados na tarefa. Mensagens em português, terminando com a linha `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Arquivos novos podem ficar com LF; o repo usa `core.autocrlf=true` e o git converte (os avisos de CRLF são esperados).
- Os comandos abaixo são PowerShell, a partir da raiz do projeto. O gerador do CMake é "Visual Studio 18 2026" (multi-config): usar `--config Release` e `.\build\Release\...`.

## Bloqueio conhecido (fora deste plano)

`RecompiledCode` **não compila** hoje com o MSVC `cl` (`__sync_bool_compare_and_swap`, `__builtin_debugtrap`, `__builtin_clzll` ausentes, e `goto` para rótulos `loc_82574740`/`loc_8257479C` definidos em outra função gerada). Por isso o executável `GreenLanternRecomp` não linka, com ou sem este trabalho. Este plano **não** tenta consertar isso. A verificação de integração usa os alvos `gpu_tests` e `gpu_selftest`, que não dependem do código recompilado. Na Tarefa 9 o executável principal é compilado com `/p:BuildProjectReferences=false` só para checar que os fontes novos compilam.

## Estrutura de arquivos

| Arquivo | Responsabilidade |
|---|---|
| `src/gpu/gpu_registers.h` | Índices de registradores e endereços MMIO |
| `src/gpu/pm4.h` | Opcodes, builders e decodificação de header PM4 |
| `src/gpu/guest_memory.h` | Acesso big-endian à memória do guest, `GpuSwap` |
| `src/gpu/gpu_backend.h` | `IGpuBackend` e `NullBackend` |
| `src/gpu/packet_stream.h` | Leitor de dwords do ring (com wraparound) e de indirect buffers |
| `src/gpu/command_processor.{h,cpp}` | Thread do CP, register file, execução dos pacotes |
| `src/gpu/interrupts.{h,cpp}` | Despacho do callback guest e thread de vblank |
| `src/gpu/gpu_system.{h,cpp}` | Estado global usado pelos `Vd*` |
| `src/gpu/selftest.{h,cpp}` | D3D simulado (chama os `Vd*`, publica swaps) |
| `src/kernel/vd_exports.cpp` | Os 20 `__imp__Vd*` |
| `src/kernel/mm_exports.cpp` | `__imp__MmGetPhysicalAddress` (identidade) |
| `src/kernel/import_stubs.cpp` | Gerado: stubs de log dos outros 198 imports |
| `src/kernel/guest_call.{h,cpp}` | Executor de funções guest para callbacks |
| `tools/gen_import_stubs.py` | Gera `import_stubs.cpp` |
| `src/graphics/renderer.{h,cpp}` | Renderer Plume: `PresentFrame` + `RendererBackend` |
| `src/gpu_selftest/main.cpp` | Executável de verificação manual |
| `tests/gpu/*` | Harness, suporte e testes |

---

### Task 1: Commit da base (renderer Plume já existente)

O working tree tem alterações não commitadas do renderer Plume (janela, swapchain, clear) em `CMakeLists.txt`, `src/graphics/renderer.cpp`, `src/graphics/renderer.h` e `src/main.cpp`. As tarefas seguintes editam esses arquivos, então o diff de cada commit só fica legível se a base entrar antes. **Pedir confirmação ao usuário antes de commitar.**

**Files:**
- Commit: `CMakeLists.txt`, `src/graphics/renderer.cpp`, `src/graphics/renderer.h`, `src/main.cpp`

**Interfaces:**
- Consumes: nada
- Produces: base commitada sobre a qual as tarefas 2 a 9 editam

- [ ] **Step 1: Confirmar com o usuário**

Perguntar: "Posso commitar as alterações atuais do renderer Plume (4 arquivos) como base antes de começar?" Se não autorizar, seguir com as tarefas e deixar esses 4 arquivos fora dos commits das tarefas 2 a 8; na Tarefa 9 eles entram junto.

- [ ] **Step 2: Conferir o que será commitado**

Run: `git status --short`
Expected: ` M CMakeLists.txt`, ` M src/graphics/renderer.cpp`, ` M src/graphics/renderer.h`, ` M src/main.cpp` (mais `?? thirdparty/`, `?? tools/XenonRecomp/`, `?? tools/XenosRecomp/`, que **não** entram).

- [ ] **Step 3: Commit**

```powershell
git add CMakeLists.txt src/graphics/renderer.cpp src/graphics/renderer.h src/main.cpp
git commit -m "Adiciona renderer Plume (Vulkan/D3D12) com janela, swapchain e clear" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Harness de teste, registradores, formato PM4 e acesso à memória do guest

**Files:**
- Create: `src/gpu/gpu_registers.h`, `src/gpu/pm4.h`, `src/gpu/guest_memory.h`
- Create: `tests/gpu/test_harness.h`, `tests/gpu/test_main.cpp`, `tests/gpu/test_pm4.cpp`
- Modify: `CMakeLists.txt` (acrescentar no fim do arquivo)

**Interfaces:**
- Consumes: `include/types.h` (`GuestAddr`, namespace `Endian::Swap32`)
- Produces:
  - `Gpu::Reg::*` (constantes) e `Gpu::Reg::MmioAddress(uint32_t reg) -> uint32_t`
  - `Gpu::Pm4::Opcode`, `kSwapSignature`, `MakeType0/2/3`, `Header`, `DecodeHeader(uint32_t) -> Header`
  - `Gpu::SwapMode`, `Gpu::GpuSwap(uint32_t, SwapMode)`, `Gpu::GuestMemory` (`Valid`, `LoadRaw32/StoreRaw32`, `LoadBe32/StoreBe32`, `StoreBe16`, `Base`)
  - Macros de teste `TEST(nome)`, `CHECK(cond)`, `CHECK_EQ(a, b)`

- [ ] **Step 1: Escrever o harness e os testes (falham por falta dos headers)**

`tests/gpu/test_harness.h`:
```cpp
#pragma once

#include <cstdio>
#include <vector>

// Harness minimo (sem framework externo). TEST(nome) { CHECK(...); } registra o caso;
// test_main.cpp roda todos e devolve exit code != 0 se algum CHECK falhar.
namespace TestHarness {
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    inline std::vector<TestCase>& Registry() {
        static std::vector<TestCase> registry;
        return registry;
    }

    struct Registrar {
        Registrar(const char* name, void (*fn)()) { Registry().push_back({ name, fn }); }
    };

    extern int g_failures;
}

#define TEST(name)                                                         \
    static void name();                                                    \
    static TestHarness::Registrar registrar_##name(#name, name);           \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("    CHECK falhou: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            ++TestHarness::g_failures;                                                 \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        const unsigned long long a_ = static_cast<unsigned long long>(actual);                  \
        const unsigned long long e_ = static_cast<unsigned long long>(expected);                \
        if (a_ != e_) {                                                                         \
            std::printf("    CHECK_EQ falhou: %s == %s (obtido 0x%llX, esperado 0x%llX) (%s:%d)\n", \
                        #actual, #expected, a_, e_, __FILE__, __LINE__);                        \
            ++TestHarness::g_failures;                                                          \
        }                                                                                       \
    } while (0)
```

`tests/gpu/test_main.cpp`:
```cpp
#include "test_harness.h"

namespace TestHarness {
    int g_failures = 0;
}

int main() {
    int failedTests = 0;
    for (const TestHarness::TestCase& test : TestHarness::Registry()) {
        TestHarness::g_failures = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        if (TestHarness::g_failures == 0) {
            std::printf("[  OK  ] %s\n", test.name);
        } else {
            std::printf("[ FAIL ] %s (%d checks)\n", test.name, TestHarness::g_failures);
            ++failedTests;
        }
    }
    std::printf("\n%zu testes, %d falharam\n", TestHarness::Registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}
```

`tests/gpu/test_pm4.cpp`:
```cpp
#include "gpu/guest_memory.h"
#include "gpu/pm4.h"
#include "test_harness.h"

using namespace Gpu;

TEST(pm4_type0_header_roundtrip) {
    const uint32_t raw = Pm4::MakeType0(0x4800, 6);
    const Pm4::Header h = Pm4::DecodeHeader(raw);
    CHECK_EQ(h.type, 0);
    CHECK_EQ(h.count, 6);
    CHECK_EQ(h.baseIndex, 0x4800);
    CHECK(!h.oneReg);
}

TEST(pm4_type0_one_reg_flag) {
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType0(0x0A31, 3, true));
    CHECK_EQ(h.type, 0);
    CHECK_EQ(h.count, 3);
    CHECK_EQ(h.baseIndex, 0x0A31);
    CHECK(h.oneReg);
}

TEST(pm4_type2_header) {
    CHECK_EQ(Pm4::MakeType2(), 0x80000000);
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType2());
    CHECK_EQ(h.type, 2);
    CHECK_EQ(h.count, 0);
}

TEST(pm4_type3_header_roundtrip) {
    const uint32_t raw = Pm4::MakeType3(Pm4::kXeSwap, 4);
    CHECK_EQ(raw, 0xC0036400);
    const Pm4::Header h = Pm4::DecodeHeader(raw);
    CHECK_EQ(h.type, 3);
    CHECK_EQ(h.count, 4);
    CHECK_EQ(h.opcode, Pm4::kXeSwap);
    CHECK(!h.predicate);
}

TEST(pm4_type3_predicate_and_max_count) {
    const Pm4::Header h = Pm4::DecodeHeader(Pm4::MakeType3(Pm4::kNop, 0x4000, true));
    CHECK_EQ(h.count, 0x4000);
    CHECK_EQ(h.opcode, Pm4::kNop);
    CHECK(h.predicate);
}

TEST(pm4_swap_signature_is_swap_fourcc) {
    CHECK_EQ(Pm4::kSwapSignature, (uint32_t('S') << 24) | (uint32_t('W') << 16) | (uint32_t('A') << 8) | uint32_t('P'));
}

TEST(gpu_swap_modes) {
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::None), 0x11223344);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k8in16), 0x22114433);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k8in32), 0x44332211);
    CHECK_EQ(GpuSwap(0x11223344, SwapMode::k16in32), 0x33441122);
}
```

- [ ] **Step 2: Acrescentar o alvo de teste ao fim do `CMakeLists.txt` raiz**

```cmake

# Testes do núcleo da GPU (executável próprio, sem framework): ctest -C Release
enable_testing()
file(GLOB GPU_TEST_SOURCES CONFIGURE_DEPENDS tests/gpu/*.cpp)
add_executable(gpu_tests ${GPU_TEST_SOURCES})
target_include_directories(gpu_tests PRIVATE tests/gpu)
add_test(NAME gpu_tests COMMAND gpu_tests)
```

- [ ] **Step 3: Rodar e ver falhar**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL de compilação, `Cannot open include file: 'gpu/pm4.h'` (e `gpu/guest_memory.h`).

- [ ] **Step 4: Implementar os três headers**

`src/gpu/gpu_registers.h`:
```cpp
#pragma once

#include <cstdint>

// Registradores da GPU Xenos usados pelo command processor. Os indices sao em dwords;
// a pagina MMIO do guest mapeia o registrador N em kMmioBase + N * 4.
namespace Gpu::Reg {
    constexpr uint32_t kRegisterCount = 0x5003;
    constexpr uint32_t kMmioBase = 0x7FC80000;
    constexpr uint32_t kMmioSize = 0x10000;

    constexpr uint32_t kCpRbWptr = 0x01C5;
    constexpr uint32_t kScratchUmsk = 0x01DC;
    constexpr uint32_t kScratchAddr = 0x01DD;
    constexpr uint32_t kScratchReg0 = 0x0578;
    constexpr uint32_t kScratchReg7 = 0x057F;
    constexpr uint32_t kCoherStatusHost = 0x0A31;
    constexpr uint32_t kVgtEventInitiator = 0x21F9;
    constexpr uint32_t kShaderConstantFetch00_0 = 0x4800;

    // Registradores que o D3D le na pagina MMIO durante a inicializacao e no vblank.
    constexpr uint32_t kRbEdramTiming = 0x0F00;
    constexpr uint32_t kRbBcControl = 0x0F01;
    constexpr uint32_t kD1ModeVCounter = 0x194C;
    constexpr uint32_t kInterruptStatus = 0x1951;
    constexpr uint32_t kD1ModeViewportSize = 0x1961;

    constexpr uint32_t MmioAddress(uint32_t reg) { return kMmioBase + reg * 4; }
}
```

`src/gpu/pm4.h`:
```cpp
#pragma once

#include <cstdint>

// Formato dos pacotes PM4 do command processor do Xenos.
namespace Gpu::Pm4 {
    // 'SWAP' (0x53574150): assinatura do pacote XE_SWAP que VdSwap escreve no ring.
    constexpr uint32_t kSwapSignature = 0x53574150;

    enum Opcode : uint32_t {
        kNop = 0x10,
        kRegRmw = 0x21,
        kDrawIndx = 0x22,
        kSetConstant = 0x2D,
        kLoadAluConstant = 0x2F,
        kDrawIndx2 = 0x36,
        kIndirectBufferPfd = 0x37,
        kMemWrite = 0x3D,
        kRegToMem = 0x3E,
        kWaitRegMem = 0x3C,
        kIndirectBuffer = 0x3F,
        kCondWrite = 0x45,
        kEventWrite = 0x46,
        kMeInit = 0x48,
        kInterrupt = 0x54,
        kSetConstant2 = 0x55,
        kSetShaderConstants = 0x56,
        kEventWriteShd = 0x58,
        kEventWriteExt = 0x5A,
        kXeSwap = 0x64,
    };

    // Tipo 0: ttcccccc cccccccc oiiiiiii iiiiiiii (o = escreve sempre o mesmo registrador)
    constexpr uint32_t MakeType0(uint32_t baseIndex, uint32_t count, bool oneReg = false) {
        return (((count - 1) & 0x3FFFu) << 16) | (oneReg ? 0x8000u : 0u) | (baseIndex & 0x7FFFu);
    }

    // Tipo 2: filler sem payload.
    constexpr uint32_t MakeType2() { return 2u << 30; }

    // Tipo 3: ttcccccc cccccccc ?ooooooo ???????p
    constexpr uint32_t MakeType3(uint32_t opcode, uint32_t count, bool predicate = false) {
        return (3u << 30) | (((count - 1) & 0x3FFFu) << 16) | ((opcode & 0x7Fu) << 8) | (predicate ? 1u : 0u);
    }

    struct Header {
        uint32_t type = 0;       // 0..3
        uint32_t count = 0;      // dwords de payload (tipos 0 e 3); 0 nos demais
        uint32_t baseIndex = 0;  // tipo 0: primeiro registrador
        bool oneReg = false;     // tipo 0: todos os dwords vao para baseIndex
        uint32_t opcode = 0;     // tipo 3
        bool predicate = false;  // tipo 3 (decodificado, ignorado no M1)
    };

    constexpr Header DecodeHeader(uint32_t raw) {
        Header h;
        h.type = raw >> 30;
        if (h.type == 0) {
            h.count = ((raw >> 16) & 0x3FFFu) + 1;
            h.baseIndex = raw & 0x7FFFu;
            h.oneReg = ((raw >> 15) & 1u) != 0;
        } else if (h.type == 3) {
            h.count = ((raw >> 16) & 0x3FFFu) + 1;
            h.opcode = (raw >> 8) & 0x7Fu;
            h.predicate = (raw & 1u) != 0;
        }
        return h;
    }
}
```

`src/gpu/guest_memory.h`:
```cpp
#pragma once

#include "types.h"
#include <cstdint>

namespace Gpu {
    // Codigo de endian nos 2 bits baixos de enderecos de escrita/leitura da GPU.
    enum class SwapMode : uint32_t {
        None = 0,
        k8in16 = 1,
        k8in32 = 2,
        k16in32 = 3,
    };

    inline uint32_t GpuSwap(uint32_t value, SwapMode mode) {
        switch (mode) {
        case SwapMode::k8in16:
            return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
        case SwapMode::k8in32:
            return ::Endian::Swap32(value);
        case SwapMode::k16in32:
            return (value >> 16) | (value << 16);
        default:
            return value;
        }
    }

    // Acesso ao espaco de enderecos do guest (memoria big-endian, base + endereco).
    // As leituras e escritas sao volatile: o guest e outras threads modificam a mesma memoria.
    class GuestMemory {
    public:
        explicit GuestMemory(uint8_t* base = nullptr) : m_base(base) {}

        uint8_t* Base() const { return m_base; }

        // Endereco nao nulo e dentro do espaco de 4 GB.
        static bool Valid(GuestAddr addr, uint64_t sizeBytes) {
            return addr != 0 && uint64_t(addr) + sizeBytes <= 0x100000000ull;
        }

        uint32_t LoadRaw32(GuestAddr addr) const {
            return *reinterpret_cast<const volatile uint32_t*>(m_base + addr);
        }
        void StoreRaw32(GuestAddr addr, uint32_t value) const {
            *reinterpret_cast<volatile uint32_t*>(m_base + addr) = value;
        }
        uint32_t LoadBe32(GuestAddr addr) const { return ::Endian::Swap32(LoadRaw32(addr)); }
        void StoreBe32(GuestAddr addr, uint32_t value) const { StoreRaw32(addr, ::Endian::Swap32(value)); }

        void StoreBe16(GuestAddr addr, uint16_t value) const {
            m_base[addr] = uint8_t(value >> 8);
            m_base[addr + 1] = uint8_t(value);
        }

    private:
        uint8_t* m_base;
    };
}
```

- [ ] **Step 5: Rodar e ver passar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: termina com `7 testes, 0 falharam`. Um warning C4267 em `ppc_context.h` pode aparecer nas tarefas seguintes; é pré-existente.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/gpu/gpu_registers.h src/gpu/pm4.h src/gpu/guest_memory.h tests/gpu/test_harness.h tests/gpu/test_main.cpp tests/gpu/test_pm4.cpp
git commit -m "Adiciona harness de teste da GPU, registradores, formato PM4 e GuestMemory" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Backend da GPU e leitor de pacotes

**Files:**
- Create: `src/gpu/gpu_backend.h`, `src/gpu/packet_stream.h`
- Create: `tests/gpu/test_support.h`, `tests/gpu/test_packet_stream.cpp`

**Interfaces:**
- Consumes: `Gpu::GuestMemory`, `Gpu::Reg::kMmioBase/kMmioSize` (Tarefa 2)
- Produces:
  - `Gpu::IGpuBackend { Swap(frontbufferAddr, width, height); OnResize(width, height); }`
  - `Gpu::NullBackend` com `std::atomic<uint32_t> swapCount, resizeCount` e `lastFrontbuffer/lastWidth/lastHeight`
  - `Gpu::PacketStream(const GuestMemory&, GuestAddr start, uint32_t capacityDwords, uint32_t readIndex, uint32_t remainingDwords)` com `Remaining()`, `ReadIndex()`, `bool Read(uint32_t*)`
  - `TestSupport::GuestBase()`, `Allocate(uint32_t) -> GuestAddr`, `WriteGuestDwords(initializer_list<uint32_t>) -> GuestAddr`, constantes `kHeapBase`, `kRingBase`, `kScratchBase`

- [ ] **Step 1: Escrever o suporte de teste e os testes**

`tests/gpu/test_support.h` (espaço de 4 GB reservado uma vez; só heap e página MMIO commitados):
```cpp
#pragma once

#include "gpu/gpu_registers.h"
#include "gpu/guest_memory.h"
#include "gpu/pm4.h"
#include "test_harness.h"

#include <cstring>
#include <initializer_list>
#include <windows.h>

namespace TestSupport {
    constexpr GuestAddr kHeapBase = 0x40000000;
    constexpr uint32_t kHeapSize = 0x04000000;  // 64 MB
    constexpr GuestAddr kRingBase = 0x40000000;
    constexpr GuestAddr kScratchBase = 0x41000000;

    // Espaco de 4 GB do guest, reservado uma vez; so o heap e a pagina MMIO sao commitados.
    inline uint8_t* GuestBase() {
        static uint8_t* base = [] {
            uint8_t* b = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE));
            VirtualAlloc(b + kHeapBase, kHeapSize, MEM_COMMIT, PAGE_READWRITE);
            VirtualAlloc(b + Gpu::Reg::kMmioBase, Gpu::Reg::kMmioSize, MEM_COMMIT, PAGE_READWRITE);
            return b;
        }();
        return base;
    }

    // Alocador bump de 4 KB para o selftest e os testes de Vd*.
    inline GuestAddr Allocate(uint32_t size) {
        static GuestAddr cursor = kScratchBase;
        const GuestAddr result = cursor;
        cursor += (size + 0xFFF) & ~0xFFFu;
        return result;
    }

    // Escreve dwords big-endian numa area nova do guest (indirect buffers, constantes) e devolve o endereco.
    inline GuestAddr WriteGuestDwords(std::initializer_list<uint32_t> words) {
        const GuestAddr addr = Allocate(0x1000);
        const Gpu::GuestMemory mem(GuestBase());
        uint32_t i = 0;
        for (uint32_t w : words) mem.StoreBe32(addr + 4 * i++, w);
        return addr;
    }
}
```

`tests/gpu/test_packet_stream.cpp`:
```cpp
#include "gpu/packet_stream.h"
#include "test_support.h"

using namespace Gpu;
using namespace TestSupport;

TEST(stream_reads_big_endian_dwords) {
    const GuestMemory mem(GuestBase());
    mem.StoreBe32(kScratchBase + 0x10, 0xAABBCCDD);
    mem.StoreBe32(kScratchBase + 0x14, 0x01020304);

    PacketStream s(mem, kScratchBase + 0x10, 8, 0, 2);
    uint32_t v = 0;
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0xAABBCCDD);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x01020304);
    CHECK_EQ(s.Remaining(), 0);
    CHECK(!s.Read(&v));
}

TEST(stream_wraps_at_capacity) {
    const GuestMemory mem(GuestBase());
    const GuestAddr base = kScratchBase + 0x100;
    for (uint32_t i = 0; i < 4; i++) mem.StoreBe32(base + i * 4, 0x100 + i);

    PacketStream s(mem, base, 4, 3, 3);  // le o indice 3, depois da a volta: 0, 1
    uint32_t v = 0;
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x103);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x100);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x101);
    CHECK_EQ(s.ReadIndex(), 2);
}

TEST(stream_rejects_null_and_overflowing_addresses) {
    const GuestMemory mem(GuestBase());
    uint32_t v = 0;

    PacketStream nullStream(mem, 0, 4, 0, 4);
    CHECK(!nullStream.Read(&v));

    PacketStream highStream(mem, 0xFFFFFFFC, 8, 1, 4);  // indice 1 -> endereco 0x1'0000'0000
    CHECK(!highStream.Read(&v));
}
```

- [ ] **Step 2: Rodar e ver falhar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL de compilação, `Cannot open include file: 'gpu/packet_stream.h'`.

- [ ] **Step 3: Implementar**

`src/gpu/gpu_backend.h`:
```cpp
#pragma once

#include <atomic>
#include <cstdint>

namespace Gpu {
    // Fronteira entre o command processor e o renderer. O CP nao conhece Plume.
    class IGpuBackend {
    public:
        virtual ~IGpuBackend() = default;

        // Chamado na thread do CP quando um pacote XE_SWAP e executado.
        virtual void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) = 0;

        // Chamado de qualquer thread (ex.: WM_SIZE). Deve ser thread-safe e so marcar a mudanca.
        virtual void OnResize(uint32_t width, uint32_t height) = 0;
    };

    // Backend sem GPU: registra as chamadas. Usado nos testes e no selftest sem janela.
    class NullBackend final : public IGpuBackend {
    public:
        void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) override {
            lastFrontbuffer = frontbufferAddr;
            lastWidth = width;
            lastHeight = height;
            swapCount.fetch_add(1, std::memory_order_release);
        }
        void OnResize(uint32_t, uint32_t) override { resizeCount.fetch_add(1, std::memory_order_release); }

        std::atomic<uint32_t> swapCount{0};
        std::atomic<uint32_t> resizeCount{0};
        // Validos depois de observar swapCount > 0 (acquire).
        uint32_t lastFrontbuffer = 0;
        uint32_t lastWidth = 0;
        uint32_t lastHeight = 0;
    };
}
```

`src/gpu/packet_stream.h`:
```cpp
#pragma once

#include "gpu/guest_memory.h"
#include <cstdint>

namespace Gpu {
    // Leitor de dwords PM4 sobre memoria do guest. Serve para o ring (com wraparound) e para
    // indirect buffers (capacidade == quantidade de dwords, logo nunca da a volta).
    class PacketStream {
    public:
        PacketStream(const GuestMemory& memory, GuestAddr start, uint32_t capacityDwords,
                     uint32_t readIndex, uint32_t remainingDwords)
            : m_memory(memory), m_start(start), m_capacity(capacityDwords),
              m_readIndex(readIndex), m_remaining(remainingDwords) {}

        uint32_t Remaining() const { return m_remaining; }
        uint32_t ReadIndex() const { return m_readIndex; }

        // Le um dword (big-endian -> host) e avanca. false se acabou ou o endereco e invalido.
        bool Read(uint32_t* out) {
            if (m_remaining == 0) return false;
            const uint64_t addr = uint64_t(m_start) + uint64_t(m_readIndex) * 4;
            if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
            *out = m_memory.LoadBe32(GuestAddr(addr));
            m_readIndex = (m_readIndex + 1 >= m_capacity) ? 0 : m_readIndex + 1;
            --m_remaining;
            return true;
        }

    private:
        GuestMemory m_memory;
        GuestAddr m_start;
        uint32_t m_capacity;
        uint32_t m_readIndex;
        uint32_t m_remaining;
    };
}
```

- [ ] **Step 4: Rodar e ver passar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: `10 testes, 0 falharam`.

- [ ] **Step 5: Commit**

```powershell
git add src/gpu/gpu_backend.h src/gpu/packet_stream.h tests/gpu/test_support.h tests/gpu/test_packet_stream.cpp
git commit -m "Adiciona IGpuBackend, NullBackend e PacketStream com testes" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Command processor (ring, pacotes PM4 e opcodes do M1)

Esta é a maior tarefa. Os testes são escritos primeiro, em três arquivos por assunto, e a implementação entra inteira (já validada). Cada grupo de testes vira um commit só no fim.

**Files:**
- Create: `src/gpu/command_processor.h`, `src/gpu/command_processor.cpp`
- Create: `tests/gpu/cp_rig.h`, `tests/gpu/test_cp_ring.cpp`, `tests/gpu/test_cp_registers.cpp`, `tests/gpu/test_cp_flow.cpp`
- Modify: `CMakeLists.txt` (biblioteca `GpuCore`)

**Interfaces:**
- Consumes: `Gpu::GuestMemory`, `Gpu::Pm4::*`, `Gpu::Reg::*`, `Gpu::PacketStream`, `Gpu::IGpuBackend`, `TestSupport::*`
- Produces:
  - `Gpu::IInterruptSink { virtual void Dispatch(uint32_t source, uint32_t cpu) = 0; }`
  - `Gpu::CpStats { packets, swaps, draws, unhandled, malformed }` (todos `uint64_t`)
  - `Gpu::CommandProcessor(uint8_t* base, IGpuBackend*, IInterruptSink*)` com `InitializeRingBuffer(GuestAddr, uint32_t sizeLog2)`, `EnableReadPointerWriteBack(GuestAddr, uint32_t)`, `Start()`, `Stop()`, `bool PumpOnce()`, `IncrementCounter()`, `Counter()`, `RegisterValue(uint32_t)`, `ReadIndex()`, `Stats()`
  - `TestSupport::CpRig` (`Emit`, `Publish`, `Submit`, membros `mem`, `backend`, `sink`, `cp`) e `TestSupport::RecordingSink`

- [ ] **Step 1: Escrever o rig e os três arquivos de teste**

`tests/gpu/cp_rig.h` (o "D3D" de teste escreve dwords com `Emit` e publica o WPTR com `Publish`/`Submit`):
```cpp
#pragma once

#include "gpu/command_processor.h"
#include "gpu/gpu_backend.h"
#include "test_support.h"

#include <vector>

namespace TestSupport {
    struct RecordingSink final : Gpu::IInterruptSink {
        struct Call { uint32_t source; uint32_t cpu; };
        void Dispatch(uint32_t source, uint32_t cpu) override { calls.push_back({ source, cpu }); }
        std::vector<Call> calls;
    };

    // Ring de teste: o "D3D" escreve dwords com Emit() e publica o WPTR com Submit().
    struct CpRig {
        explicit CpRig(uint32_t sizeLog2 = 8)
            : mem(GuestBase()), cp(GuestBase(), &backend, &sink) {
            ringDwords = (1u << (sizeLog2 + 3)) / 4;
            std::memset(GuestBase() + kRingBase, 0, ringDwords * 4);
            mem.StoreBe32(Gpu::Reg::MmioAddress(Gpu::Reg::kCpRbWptr), 0);
            cp.InitializeRingBuffer(kRingBase, sizeLog2);
        }

        void Emit(std::initializer_list<uint32_t> words) {
            for (uint32_t w : words) {
                mem.StoreBe32(kRingBase + wptr * 4, w);
                wptr = (wptr + 1) % ringDwords;
            }
        }

        // Publica o WPTR na pagina MMIO (o que o D3D faz depois de escrever os pacotes).
        void Publish() { mem.StoreBe32(Gpu::Reg::MmioAddress(Gpu::Reg::kCpRbWptr), wptr); }

        // Publica o WPTR e deixa o CP processar (sincrono, sem thread).
        bool Submit() {
            Publish();
            return cp.PumpOnce();
        }

        Gpu::GuestMemory mem;
        Gpu::NullBackend backend;
        RecordingSink sink;
        Gpu::CommandProcessor cp;
        uint32_t ringDwords = 0;
        uint32_t wptr = 0;
    };
}
```

`tests/gpu/test_cp_ring.cpp`:
```cpp
#include "cp_rig.h"

using namespace Gpu;
using namespace TestSupport;

namespace {
    constexpr uint32_t kType2 = 0x80000000;
}

// --- Decodificacao de pacotes, ring e write-back ---------------------------------------------

TEST(cp_idle_when_wptr_equals_rptr) {
    CpRig rig;
    CHECK(!rig.Submit());
    CHECK_EQ(rig.cp.ReadIndex(), 0);
}

TEST(cp_type0_writes_consecutive_registers) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 3), 0x11, 0x22, 0x33 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x11);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0x22);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0x33);
    CHECK_EQ(rig.cp.ReadIndex(), 4);
}

TEST(cp_type0_one_reg_writes_same_register) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 3, true), 1, 2, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 3);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0);
}

TEST(cp_wraps_around_ring_end) {
    CpRig rig(4);  // ring de 32 dwords
    for (int i = 0; i < 30; i++) rig.Emit({ kType2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.ReadIndex(), 30);

    rig.Emit({ Pm4::MakeType0(0x4800, 3), 0xA, 0xB, 0xC });  // ocupa os indices 30, 31, 0, 1
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0xA);
    CHECK_EQ(rig.cp.RegisterValue(0x4801), 0xB);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0xC);
    CHECK_EQ(rig.cp.ReadIndex(), 2);
}

TEST(cp_type2_and_zero_headers_are_skipped) {
    CpRig rig;
    rig.Emit({ kType2, 0, kType2, Pm4::MakeType0(0x4800, 1), 7 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 7);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_writes_back_read_pointer_big_endian) {
    CpRig rig;
    const GuestAddr writeback = Allocate(0x1000);
    rig.cp.EnableReadPointerWriteBack(writeback, 6);
    rig.Emit({ kType2, kType2, kType2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(writeback), 3);
}

TEST(cp_type1_packet_is_malformed_and_dropped_until_wptr) {
    CpRig rig;
    rig.Emit({ 1u << 30, 0, 0, Pm4::MakeType0(0x4800, 1), 9 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0);  // descartado junto com o resto do lote
    CHECK_EQ(rig.cp.ReadIndex(), 5);

    rig.Emit({ Pm4::MakeType0(0x4800, 1), 5 });  // o CP continua vivo
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 5);
}

TEST(cp_type0_overflowing_the_batch_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x4800, 8), 1, 2 });  // declara 8 dwords, so ha 2
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0);
}

TEST(cp_unknown_opcode_is_skipped_without_losing_sync) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(0x7E, 3), 1, 2, 3, Pm4::MakeType0(0x4800, 1), 0x55 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().unhandled, 1);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x55);
}

// --- Opcodes de registrador e memoria ---------------------------------------------------------
```

`tests/gpu/test_cp_registers.cpp`:
```cpp
#include "cp_rig.h"

using namespace Gpu;
using namespace TestSupport;

// --- Opcodes de registrador e memoria ---------------------------------------------------------

TEST(cp_set_constant_maps_each_type_to_its_register_base) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 3), (0u << 16) | 4, 0x1111, 0x2222 });  // ALU
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (1u << 16) | 2, 0x3333 });          // FETCH
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (2u << 16) | 0, 0x4444 });          // BOOL
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (3u << 16) | 1, 0x5555 });          // LOOP
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant, 2), (4u << 16) | 1, 0x6666 });          // REGISTERS
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4004), 0x1111);
    CHECK_EQ(rig.cp.RegisterValue(0x4005), 0x2222);
    CHECK_EQ(rig.cp.RegisterValue(0x4802), 0x3333);
    CHECK_EQ(rig.cp.RegisterValue(0x4900), 0x4444);
    CHECK_EQ(rig.cp.RegisterValue(0x4909), 0x5555);
    CHECK_EQ(rig.cp.RegisterValue(0x2001), 0x6666);
}

TEST(cp_set_constant2_and_set_shader_constants_use_absolute_index) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kSetConstant2, 3), 0x4810, 0xAAAA, 0xBBBB });
    rig.Emit({ Pm4::MakeType3(Pm4::kSetShaderConstants, 2), 0x4000, 0xCCCC });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4810), 0xAAAA);
    CHECK_EQ(rig.cp.RegisterValue(0x4811), 0xBBBB);
    CHECK_EQ(rig.cp.RegisterValue(0x4000), 0xCCCC);
}

TEST(cp_load_alu_constant_reads_guest_memory) {
    CpRig rig;
    const GuestAddr source = WriteGuestDwords({ 0xDEAD0001, 0xDEAD0002 });
    rig.Emit({ Pm4::MakeType3(Pm4::kLoadAluConstant, 3), source, (0u << 16) | 2, 2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4002), 0xDEAD0001);
    CHECK_EQ(rig.cp.RegisterValue(0x4003), 0xDEAD0002);
}

TEST(cp_reg_rmw_with_immediate_masks) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0xFF00 });
    rig.Emit({ Pm4::MakeType3(Pm4::kRegRmw, 3), 0x0800, 0x0FF0, 0x000F });  // (0xFF00 & 0x0FF0) | 0xF
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x0800), 0x0F0F);
}

TEST(cp_reg_rmw_with_register_operands) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0xABCD });
    rig.Emit({ Pm4::MakeType0(0x0801, 1), 0x00FF });
    rig.Emit({ Pm4::MakeType0(0x0802, 1), 0x1000 });
    // bit 31: operando AND vem do registrador 0x0801; bit 30: operando OR vem do registrador 0x0802
    rig.Emit({ Pm4::MakeType3(Pm4::kRegRmw, 3), 0xC0000000 | 0x0800, 0x0801, 0x0802 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x0800), (0xABCD & 0x00FF) | 0x1000);
}

TEST(cp_reg_to_mem_swaps_by_address_bits) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0x11223344 });
    rig.Emit({ Pm4::MakeType3(Pm4::kRegToMem, 2), 0x0800, target | 2 });  // 8in32
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(target), 0x11223344);
}

TEST(cp_mem_write_writes_consecutive_dwords) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kMemWrite, 3), target | 2, 0xAABBCCDD, 0x01020304 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(target), 0xAABBCCDD);
    CHECK_EQ(rig.mem.LoadBe32(target + 4), 0x01020304);
}

TEST(cp_cond_write_to_memory_only_when_condition_matches) {
    CpRig rig;
    const GuestAddr hit = Allocate(0x1000);
    const GuestAddr miss = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(0x0800, 1), 0x1234 });
    // 0x103 = "igual" (3) + escrita em memoria (0x100); poll no registrador 0x0800
    rig.Emit({ Pm4::MakeType3(Pm4::kCondWrite, 6), 0x103, 0x0800, 0x1234, 0xFFFF, hit | 2, 0xCAFE });
    rig.Emit({ Pm4::MakeType3(Pm4::kCondWrite, 6), 0x103, 0x0800, 0x9999, 0xFFFF, miss | 2, 0xCAFE });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(hit), 0xCAFE);
    CHECK_EQ(rig.mem.LoadBe32(miss), 0);
}

TEST(cp_event_write_sets_initiator_register) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWrite, 1), 0x4000003F | 0x80 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(Reg::kVgtEventInitiator), 0x3F);
}

TEST(cp_event_write_shd_writes_value_then_counter) {
    CpRig rig;
    const GuestAddr fence = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteShd, 3), 0x00000005, fence | 2, 0xFEED });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(fence), 0xFEED);
    CHECK_EQ(rig.cp.RegisterValue(Reg::kVgtEventInitiator), 5);

    for (int i = 0; i < 7; i++) rig.cp.IncrementCounter();
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteShd, 3), 0x80000005, fence | 2, 0xFEED });  // bit 31: grava o contador
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(fence), 7);
}

TEST(cp_event_write_ext_writes_big_endian_extents) {
    CpRig rig;
    const GuestAddr target = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kEventWriteExt, 2), 0x1, target | 1 });
    CHECK(rig.Submit());
    const uint8_t* b = GuestBase() + target;
    CHECK_EQ((b[0] << 8) | b[1], 0);
    CHECK_EQ((b[2] << 8) | b[3], 1024);
    CHECK_EQ((b[6] << 8) | b[7], 1024);
    CHECK_EQ((b[10] << 8) | b[11], 1);
}

TEST(cp_scratch_registers_mirror_to_memory_when_enabled) {
    CpRig rig;
    const GuestAddr scratch = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType0(Reg::kScratchAddr, 1), scratch });
    rig.Emit({ Pm4::MakeType0(Reg::kScratchUmsk, 1), 0x2 });  // so o slot 1
    rig.Emit({ Pm4::MakeType0(Reg::kScratchReg0, 2), 5, 9 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.mem.LoadBe32(scratch), 0);
    CHECK_EQ(rig.mem.LoadBe32(scratch + 4), 9);
}

TEST(cp_draw_opcodes_are_counted_not_executed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx, 3), 1, 2, 3 });
    rig.Emit({ Pm4::MakeType3(Pm4::kDrawIndx2, 2), 4, 5 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().draws, 2);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

// --- Indirect buffers -------------------------------------------------------------------------
```

`tests/gpu/test_cp_flow.cpp`:
```cpp
#include "cp_rig.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace Gpu;
using namespace TestSupport;

// --- Indirect buffers, WAIT_REG_MEM, swap, interrupcoes e thread -------------------------------

TEST(cp_indirect_buffer_executes_nested_packets) {
    CpRig rig;
    const GuestAddr ib = WriteGuestDwords({ Pm4::MakeType0(0x4800, 1), 0x77 });
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), ib, 2 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x77);
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_indirect_buffer_can_chain_another_indirect_buffer) {
    CpRig rig;
    const GuestAddr inner = WriteGuestDwords({ Pm4::MakeType0(0x4800, 1), 0x88 });
    const GuestAddr outer = WriteGuestDwords({ Pm4::MakeType3(Pm4::kIndirectBufferPfd, 2), inner, 2 });
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), outer, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 0x88);
}

TEST(cp_self_referencing_indirect_buffer_hits_depth_limit) {
    CpRig rig;
    const GuestAddr loop = Allocate(0x1000);
    rig.mem.StoreBe32(loop, Pm4::MakeType3(Pm4::kIndirectBuffer, 2));
    rig.mem.StoreBe32(loop + 4, loop);
    rig.mem.StoreBe32(loop + 8, 3);
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), loop, 3 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);  // o CP sobrevive
}

TEST(cp_indirect_buffer_with_null_pointer_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kIndirectBuffer, 2), 0, 4 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.Stats().malformed, 1);
}

// --- WAIT_REG_MEM -----------------------------------------------------------------------------

TEST(cp_wait_reg_mem_memory_completes_when_value_arrives) {
    CpRig rig;
    const GuestAddr flag = Allocate(0x1000);
    // 0x13 = "igual" (3) + polling em memoria (0x10); o endereco carrega o modo 8in32
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x13, flag | 2, 1, 0xFFFFFFFF, 0 });
    rig.Emit({ Pm4::MakeType0(0x4800, 1), 3 });

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        rig.mem.StoreBe32(flag, 1);
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(rig.Submit());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    writer.join();

    CHECK(elapsed >= std::chrono::milliseconds(20));
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 3);  // o pacote depois do WAIT executou
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

TEST(cp_wait_reg_mem_register_synchronizes_coherency_and_never_hangs) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType0(Reg::kCoherStatusHost, 1), 0x01000000 });  // marca pendente (bit 31)
    // espera COHER_STATUS_HOST == 0: so vale depois que o CP sincroniza
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x3, Reg::kCoherStatusHost, 0, 0xFFFFFFFF, 0 });
    // registrador que nunca vai satisfazer a condicao: ignorado, sem travar o CP
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x3, 0x0800, 5, 0xFFFFFFFF, 0 });
    rig.Emit({ Pm4::MakeType0(0x4800, 1), 4 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.cp.RegisterValue(Reg::kCoherStatusHost), 0);
    CHECK_EQ(rig.cp.RegisterValue(0x4800), 4);
}

TEST(cp_stop_aborts_a_wait_that_never_completes) {
    CpRig rig;
    const GuestAddr flag = Allocate(0x1000);
    rig.Emit({ Pm4::MakeType3(Pm4::kWaitRegMem, 5), 0x13, flag | 2, 1, 0xFFFFFFFF, 0 });
    rig.Publish();
    rig.cp.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // o CP entra no WAIT
    rig.cp.Stop();                                               // deve retornar (join)
    CHECK_EQ(rig.cp.Stats().malformed, 0);
}

// --- Swap, interrupcoes e thread --------------------------------------------------------------

TEST(cp_xe_swap_calls_backend_with_frontbuffer_and_size) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), Pm4::kSwapSignature, 0x40100000, 1280, 720 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.swapCount.load(), 1);
    CHECK_EQ(rig.backend.lastFrontbuffer, 0x40100000);
    CHECK_EQ(rig.backend.lastWidth, 1280);
    CHECK_EQ(rig.backend.lastHeight, 720);
    CHECK_EQ(rig.cp.Stats().swaps, 1);
    CHECK_EQ(rig.cp.Counter(), 1);
}

TEST(cp_xe_swap_with_bad_signature_is_malformed) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), 0x12345678, 0x40100000, 1280, 720 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.backend.swapCount.load(), 0);
    CHECK_EQ(rig.cp.Stats().malformed, 1);
}

TEST(cp_interrupt_dispatches_once_per_cpu_in_mask) {
    CpRig rig;
    rig.Emit({ Pm4::MakeType3(Pm4::kInterrupt, 1), 0b101 });
    CHECK(rig.Submit());
    CHECK_EQ(rig.sink.calls.size(), 2);
    CHECK_EQ(rig.sink.calls[0].source, 1);
    CHECK_EQ(rig.sink.calls[0].cpu, 0);
    CHECK_EQ(rig.sink.calls[1].cpu, 2);
}

TEST(cp_thread_consumes_ring_published_by_the_guest) {
    CpRig rig;
    rig.cp.Start();
    rig.Emit({ Pm4::MakeType3(Pm4::kXeSwap, 4), Pm4::kSwapSignature, 0x40100000, 1280, 720 });
    rig.Publish();  // o D3D so escreve o WPTR; a thread do CP faz o polling

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.backend.swapCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rig.cp.Stop();
    CHECK_EQ(rig.backend.swapCount.load(), 1);
    CHECK_EQ(rig.backend.lastWidth, 1280);
}
```

- [ ] **Step 2: Criar a biblioteca `GpuCore` no `CMakeLists.txt` raiz**

Inserir antes da linha `# Definição do Executável Principal`:

```cmake
# Núcleo da GPU emulada: command processor PM4, interrupções e os imports Vd*/Mm* com implementação real.
# Não depende de Plume nem do código recompilado (só de ppc_context.h), então os testes linkam rápido.
file(GLOB GPU_CORE_SOURCES CONFIGURE_DEPENDS src/gpu/*.cpp src/kernel/*_exports.cpp)
add_library(GpuCore STATIC ${GPU_CORE_SOURCES})

```

E, logo depois de `add_executable(gpu_tests ${GPU_TEST_SOURCES})`, acrescentar:

```cmake
target_link_libraries(gpu_tests PRIVATE GpuCore)
```

- [ ] **Step 3: Rodar e ver falhar**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL. `GpuCore` ainda não tem fontes (`No SOURCES given to target: GpuCore`) ou, se já houver, `Cannot open include file: 'gpu/command_processor.h'`.

- [ ] **Step 4: Implementar o command processor**

`src/gpu/command_processor.h`:
```cpp
#pragma once

#include "gpu/gpu_backend.h"
#include "gpu/gpu_registers.h"
#include "gpu/guest_memory.h"
#include "gpu/packet_stream.h"
#include "gpu/pm4.h"

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace Gpu {
    // Destino das interrupcoes geradas pelo CP (pacote INTERRUPT).
    class IInterruptSink {
    public:
        virtual ~IInterruptSink() = default;
        virtual void Dispatch(uint32_t source, uint32_t cpu) = 0;
    };

    struct CpStats {
        uint64_t packets = 0;
        uint64_t swaps = 0;
        uint64_t draws = 0;      // DRAW_INDX* contados, nao executados no M1
        uint64_t unhandled = 0;  // opcodes type 3 pulados pelo tamanho
        uint64_t malformed = 0;  // pacotes invalidos (descartados ate o WPTR)
    };

    // Emula o command processor do Xenos: consome o ring buffer PM4 escrito pelo D3D do jogo.
    class CommandProcessor {
    public:
        CommandProcessor(uint8_t* base, IGpuBackend* backend, IInterruptSink* interrupts);
        ~CommandProcessor();
        CommandProcessor(const CommandProcessor&) = delete;
        CommandProcessor& operator=(const CommandProcessor&) = delete;

        // sizeLog2: o ring tem (1 << (sizeLog2 + 3)) bytes (convencao de VdInitializeRingBuffer).
        void InitializeRingBuffer(GuestAddr ptr, uint32_t sizeLog2);
        // O RPTR e escrito (big-endian) em ptr depois de cada lote. blockSizeLog2 nao e usado.
        void EnableReadPointerWriteBack(GuestAddr ptr, uint32_t blockSizeLog2);

        void Start();
        void Stop();

        // Executa o que o guest publicou no WPTR (pagina MMIO). true se processou algo.
        // E o que a thread do CP chama em loop; os testes chamam direto.
        bool PumpOnce();

        void IncrementCounter() { m_counter.fetch_add(1, std::memory_order_relaxed); }
        uint32_t Counter() const { return m_counter.load(std::memory_order_relaxed); }
        uint32_t RegisterValue(uint32_t index) const { return index < Reg::kRegisterCount ? m_regs[index] : 0; }
        uint32_t ReadIndex() const { return m_readIndex.load(std::memory_order_acquire); }
        CpStats Stats() const;

    private:
        static constexpr int kMaxIndirectDepth = 8;

        void WorkerMain();
        uint32_t ExecutePrimaryBuffer(GuestAddr ringBase, uint32_t ringDwords, uint32_t readIndex, uint32_t writeIndex);
        bool ExecuteIndirectBuffer(GuestAddr ptr, uint32_t countDwords, int depth);
        bool ExecutePacket(PacketStream& stream, int depth);
        bool ExecuteType0(PacketStream& stream, const Pm4::Header& header);
        bool ExecuteType3(PacketStream& stream, const Pm4::Header& header, int depth);
        bool ExecuteWaitRegMem(const uint32_t* d, uint32_t n);
        bool ExecuteCondWrite(const uint32_t* d, uint32_t n);
        bool ExecuteEventWriteExt(const uint32_t* d, uint32_t n);
        bool ExecuteLoadAluConstant(const uint32_t* d, uint32_t n);

        void WriteRegister(uint32_t index, uint32_t value);
        bool LoadSwapped(GuestAddr addrWithMode, uint32_t* out) const;
        bool StoreSwapped(GuestAddr addrWithMode, uint32_t value) const;
        void WriteBackReadPointer();
        void LogOnce(std::set<uint32_t>& seen, uint32_t key, const char* what, uint32_t detail);

        GuestMemory m_memory;
        IGpuBackend* m_backend;
        IInterruptSink* m_interrupts;

        std::array<uint32_t, Reg::kRegisterCount> m_regs{};

        std::mutex m_ringMutex;  // protege base/tamanho do ring contra re-inicializacao pelo guest
        GuestAddr m_ringBase = 0;
        uint32_t m_ringDwords = 0;
        std::atomic<uint32_t> m_readIndex{0};
        std::atomic<GuestAddr> m_rptrWriteback{0};

        std::atomic<uint32_t> m_counter{0};
        std::atomic<bool> m_stop{false};
        std::thread m_thread;

        std::atomic<uint64_t> m_packets{0}, m_swaps{0}, m_draws{0}, m_unhandled{0}, m_malformed{0};
        std::set<uint32_t> m_loggedOpcodes;
        std::set<uint32_t> m_loggedMisc;
    };
}
```

`src/gpu/command_processor.cpp`:
```cpp
#include "gpu/command_processor.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

namespace Gpu {
    namespace {
        constexpr uint32_t kMaxRingSizeLog2 = 24;

        // Base do register file para cada tipo de constante em SET_CONSTANT / LOAD_ALU_CONSTANT.
        bool ConstantBase(uint32_t type, uint32_t* base) {
            switch (type) {
            case 0: *base = 0x4000; return true;  // ALU
            case 1: *base = 0x4800; return true;  // FETCH
            case 2: *base = 0x4900; return true;  // BOOL
            case 3: *base = 0x4908; return true;  // LOOP
            case 4: *base = 0x2000; return true;  // REGISTERS
            default: return false;
            }
        }

        bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
            switch (function & 7) {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }
    }

    CommandProcessor::CommandProcessor(uint8_t* base, IGpuBackend* backend, IInterruptSink* interrupts)
        : m_memory(base), m_backend(backend), m_interrupts(interrupts) {}

    CommandProcessor::~CommandProcessor() { Stop(); }

    void CommandProcessor::InitializeRingBuffer(GuestAddr ptr, uint32_t sizeLog2) {
        if (sizeLog2 > kMaxRingSizeLog2 || !GuestMemory::Valid(ptr, uint64_t(1) << (sizeLog2 + 3))) {
            std::cerr << "[GPU] VdInitializeRingBuffer invalido: ptr=0x" << std::hex << ptr
                      << " size_log2=" << std::dec << sizeLog2 << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_ringBase = ptr;
        m_ringDwords = (uint32_t(1) << (sizeLog2 + 3)) / 4;
        m_readIndex.store(0, std::memory_order_release);
    }

    void CommandProcessor::EnableReadPointerWriteBack(GuestAddr ptr, uint32_t) {
        m_rptrWriteback.store(ptr, std::memory_order_release);
    }

    void CommandProcessor::Start() {
        if (m_thread.joinable()) return;
        m_stop.store(false);
        m_thread = std::thread([this] { WorkerMain(); });
    }

    void CommandProcessor::Stop() {
        m_stop.store(true);
        if (m_thread.joinable()) m_thread.join();
    }

    CpStats CommandProcessor::Stats() const {
        CpStats s;
        s.packets = m_packets.load();
        s.swaps = m_swaps.load();
        s.draws = m_draws.load();
        s.unhandled = m_unhandled.load();
        s.malformed = m_malformed.load();
        return s;
    }

    void CommandProcessor::WorkerMain() {
        uint32_t idle = 0;
        while (!m_stop.load()) {
            if (PumpOnce()) {
                idle = 0;
                continue;
            }
            // Sem comandos: spin curto, depois espera de ~100 us para nao ocupar um nucleo.
            if (++idle < 200) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    bool CommandProcessor::PumpOnce() {
        GuestAddr ringBase;
        uint32_t ringDwords;
        {
            std::lock_guard<std::mutex> lock(m_ringMutex);
            ringBase = m_ringBase;
            ringDwords = m_ringDwords;
        }
        if (ringBase == 0 || ringDwords == 0) return false;

        const uint32_t writeIndex = m_memory.LoadBe32(Reg::MmioAddress(Reg::kCpRbWptr)) % ringDwords;
        const uint32_t readIndex = m_readIndex.load(std::memory_order_acquire);
        if (readIndex == writeIndex) return false;

        const uint32_t newRead = ExecutePrimaryBuffer(ringBase, ringDwords, readIndex, writeIndex);
        m_readIndex.store(newRead, std::memory_order_release);
        WriteBackReadPointer();
        return true;
    }

    uint32_t CommandProcessor::ExecutePrimaryBuffer(GuestAddr ringBase, uint32_t ringDwords,
                                                    uint32_t readIndex, uint32_t writeIndex) {
        const uint32_t pending = (writeIndex + ringDwords - readIndex) % ringDwords;
        PacketStream stream(m_memory, ringBase, ringDwords, readIndex, pending);

        while (stream.Remaining() > 0) {
            if (!ExecutePacket(stream, 0)) {
                if (!m_stop.load()) {
                    m_malformed.fetch_add(1);
                    LogOnce(m_loggedMisc, 0xFFFF0000u, "pacote PM4 invalido no ring; descartando ate o WPTR, indice",
                            stream.ReadIndex());
                }
                break;  // descarta o resto ate o WPTR
            }
        }
        return writeIndex;
    }

    bool CommandProcessor::ExecuteIndirectBuffer(GuestAddr ptr, uint32_t countDwords, int depth) {
        if (depth >= kMaxIndirectDepth) return false;
        if (countDwords == 0) return true;
        if (!GuestMemory::Valid(ptr, uint64_t(countDwords) * 4)) return false;

        PacketStream stream(m_memory, ptr, countDwords, 0, countDwords);
        while (stream.Remaining() > 0) {
            if (!ExecutePacket(stream, depth + 1)) return false;
        }
        return true;
    }

    bool CommandProcessor::ExecutePacket(PacketStream& stream, int depth) {
        uint32_t raw;
        if (!stream.Read(&raw)) return false;
        m_packets.fetch_add(1, std::memory_order_relaxed);

        if (raw == 0) return true;  // padding: nao carrega payload

        const Pm4::Header header = Pm4::DecodeHeader(raw);
        switch (header.type) {
        case 0: return ExecuteType0(stream, header);
        case 2: return true;
        case 3: return ExecuteType3(stream, header, depth);
        default: return false;  // tipo 1 nao e emitido pelo D3D do 360
        }
    }

    bool CommandProcessor::ExecuteType0(PacketStream& stream, const Pm4::Header& header) {
        if (stream.Remaining() < header.count) return false;
        for (uint32_t i = 0; i < header.count; i++) {
            uint32_t value;
            if (!stream.Read(&value)) return false;
            WriteRegister(header.oneReg ? header.baseIndex : header.baseIndex + i, value);
        }
        return true;
    }

    bool CommandProcessor::ExecuteType3(PacketStream& stream, const Pm4::Header& header, int depth) {
        const uint32_t n = header.count;
        if (stream.Remaining() < n) return false;

        // O payload e lido por inteiro antes do handler: o ring fica consistente mesmo para
        // opcodes desconhecidos ou que consomem menos dwords do que o pacote declara.
        uint32_t small[64];
        std::vector<uint32_t> large;
        uint32_t* d = small;
        if (n > 64) {
            large.resize(n);
            d = large.data();
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!stream.Read(&d[i])) return false;
        }

        switch (header.opcode) {
        case Pm4::kNop:
        case Pm4::kMeInit:
            return true;

        case Pm4::kInterrupt: {
            if (n < 1) return false;
            if (m_interrupts) {
                for (uint32_t cpu = 0; cpu < 6; cpu++) {
                    if (d[0] & (1u << cpu)) m_interrupts->Dispatch(1, cpu);
                }
            }
            return true;
        }

        case Pm4::kXeSwap: {
            if (n < 4 || d[0] != Pm4::kSwapSignature) return false;
            m_backend->Swap(d[1], d[2], d[3]);
            m_swaps.fetch_add(1);
            IncrementCounter();
            return true;
        }

        case Pm4::kIndirectBuffer:
        case Pm4::kIndirectBufferPfd: {
            if (n < 2) return false;
            const GuestAddr ptr = d[0];
            const uint32_t count = d[1] & 0xFFFFF;
            return ExecuteIndirectBuffer(ptr, count, depth);
        }

        case Pm4::kWaitRegMem: return ExecuteWaitRegMem(d, n);
        case Pm4::kCondWrite: return ExecuteCondWrite(d, n);
        case Pm4::kEventWriteExt: return ExecuteEventWriteExt(d, n);
        case Pm4::kLoadAluConstant: return ExecuteLoadAluConstant(d, n);

        case Pm4::kRegRmw: {
            if (n < 3) return false;
            const uint32_t index = d[0] & 0x1FFF;
            uint32_t value = m_regs[index];
            value &= ((d[0] >> 31) & 1u) ? m_regs[d[1] & 0x1FFF] : d[1];
            value |= ((d[0] >> 30) & 1u) ? m_regs[d[2] & 0x1FFF] : d[2];
            WriteRegister(index, value);
            return true;
        }

        case Pm4::kRegToMem: {
            if (n < 2 || d[0] >= Reg::kRegisterCount) return false;
            return StoreSwapped(d[1], m_regs[d[0]]);
        }

        case Pm4::kMemWrite: {
            if (n < 1) return false;
            const SwapMode mode = SwapMode(d[0] & 3);
            const GuestAddr base = d[0] & ~GuestAddr(3);
            for (uint32_t i = 0; i + 1 < n; i++) {
                const uint64_t addr = uint64_t(base) + uint64_t(i) * 4;
                if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
                m_memory.StoreRaw32(GuestAddr(addr), GpuSwap(d[1 + i], mode));
            }
            return true;
        }

        case Pm4::kEventWrite: {
            if (n < 1) return false;
            WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);
            return true;
        }

        case Pm4::kEventWriteShd: {
            if (n < 3) return false;
            WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);
            const uint32_t value = ((d[0] >> 31) & 1u) ? Counter() : d[2];
            return StoreSwapped(d[1], value);
        }

        case Pm4::kSetConstant: {
            if (n < 1) return false;
            uint32_t base;
            if (!ConstantBase((d[0] >> 16) & 0xFF, &base)) return true;  // tipo desconhecido: pula
            const uint32_t index = base + (d[0] & 0x7FF);
            for (uint32_t i = 1; i < n; i++) WriteRegister(index + i - 1, d[i]);
            return true;
        }

        case Pm4::kSetConstant2:
        case Pm4::kSetShaderConstants: {
            if (n < 1) return false;
            const uint32_t index = d[0] & 0xFFFF;
            for (uint32_t i = 1; i < n; i++) WriteRegister(index + i - 1, d[i]);
            return true;
        }

        case Pm4::kDrawIndx:
        case Pm4::kDrawIndx2:
            m_draws.fetch_add(1);
            return true;

        default:
            m_unhandled.fetch_add(1);
            LogOnce(m_loggedOpcodes, header.opcode, "opcode PM4 sem suporte no M1 (pulado), opcode", header.opcode);
            return true;
        }
    }

    bool CommandProcessor::ExecuteWaitRegMem(const uint32_t* d, uint32_t n) {
        if (n < 5) return false;
        const uint32_t waitInfo = d[0];
        const uint32_t pollAddr = d[1];
        const uint32_t ref = d[2];
        const uint32_t mask = d[3];
        const uint32_t wait = d[4];

        if ((waitInfo & 0x10) == 0) {
            // Registrador: so o proprio CP altera o register file, entao a condicao nao pode mudar
            // enquanto esperamos. Avalia uma vez (apos o coherency do host) e segue adiante.
            if (pollAddr >= Reg::kRegisterCount) return false;
            if (pollAddr == Reg::kCoherStatusHost && (m_regs[pollAddr] & 0x80000000u)) {
                m_regs[pollAddr] = 0;
            }
            if (!Compare(waitInfo, m_regs[pollAddr] & mask, ref)) {
                LogOnce(m_loggedMisc, 0xFFFF0001u, "WAIT_REG_MEM em registrador nunca satisfeito (ignorado), registrador", pollAddr);
            }
            return true;
        }

        // Memoria: espera ate outra thread (guest) satisfazer a condicao, ou ate Stop().
        while (true) {
            uint32_t value;
            if (!LoadSwapped(pollAddr, &value)) return false;
            if (Compare(waitInfo, value & mask, ref)) return true;
            if (m_stop.load()) return false;
            if (wait >= 0x100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
            } else {
                std::this_thread::yield();
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    bool CommandProcessor::ExecuteCondWrite(const uint32_t* d, uint32_t n) {
        if (n < 6) return false;
        const uint32_t waitInfo = d[0];
        uint32_t value;
        if (waitInfo & 0x10) {
            if (!LoadSwapped(d[1], &value)) return false;
        } else {
            if (d[1] >= Reg::kRegisterCount) return false;
            value = m_regs[d[1]];
        }
        if (!Compare(waitInfo, value & d[3], d[2])) return true;

        if (waitInfo & 0x100) return StoreSwapped(d[4], d[5]);
        WriteRegister(d[4], d[5]);
        return true;
    }

    bool CommandProcessor::ExecuteEventWriteExt(const uint32_t* d, uint32_t n) {
        if (n < 2) return false;
        WriteRegister(Reg::kVgtEventInitiator, d[0] & 0x3F);

        // O driver le as extensoes (x/y/z min/max) afetadas pelo ultimo draw; devolvemos a tela toda.
        const GuestAddr addr = d[1] & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 12)) return false;
        const uint16_t extents[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
        for (uint32_t i = 0; i < 6; i++) m_memory.StoreBe16(addr + i * 2, extents[i]);
        return true;
    }

    bool CommandProcessor::ExecuteLoadAluConstant(const uint32_t* d, uint32_t n) {
        if (n < 3) return false;
        // Endereco fisico == virtual neste port (MmGetPhysicalAddress e a identidade): sem mascara.
        const GuestAddr address = d[0];
        const uint32_t size = d[2] & 0xFFF;
        uint32_t base;
        if (!ConstantBase((d[1] >> 16) & 0xFF, &base)) return true;
        const uint32_t index = base + (d[1] & 0x7FF);
        for (uint32_t i = 0; i < size; i++) {
            const uint64_t addr = uint64_t(address) + uint64_t(i) * 4;
            if (addr > 0xFFFFFFFFull || !GuestMemory::Valid(GuestAddr(addr), 4)) return false;
            WriteRegister(index + i, m_memory.LoadBe32(GuestAddr(addr)));
        }
        return true;
    }

    void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
        if (index >= Reg::kRegisterCount) {
            LogOnce(m_loggedMisc, 0xFFFF0002u, "escrita em registrador fora do range (ignorada), indice", index);
            return;
        }
        m_regs[index] = value;

        if (index >= Reg::kScratchReg0 && index <= Reg::kScratchReg7) {
            // Registradores scratch espelhados em memoria quando habilitados em SCRATCH_UMSK.
            const uint32_t slot = index - Reg::kScratchReg0;
            if (m_regs[Reg::kScratchUmsk] & (1u << slot)) {
                const GuestAddr addr = m_regs[Reg::kScratchAddr] + slot * 4;
                if (GuestMemory::Valid(addr, 4)) m_memory.StoreBe32(addr, value);
            }
        } else if (index == Reg::kCoherStatusHost) {
            m_regs[index] |= 0x80000000u;  // pendente: o proximo WAIT_REG_MEM sincroniza
        }
    }

    bool CommandProcessor::LoadSwapped(GuestAddr addrWithMode, uint32_t* out) const {
        const GuestAddr addr = addrWithMode & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 4)) return false;
        *out = GpuSwap(m_memory.LoadRaw32(addr), SwapMode(addrWithMode & 3));
        return true;
    }

    bool CommandProcessor::StoreSwapped(GuestAddr addrWithMode, uint32_t value) const {
        const GuestAddr addr = addrWithMode & ~GuestAddr(3);
        if (!GuestMemory::Valid(addr, 4)) return false;
        m_memory.StoreRaw32(addr, GpuSwap(value, SwapMode(addrWithMode & 3)));
        return true;
    }

    void CommandProcessor::WriteBackReadPointer() {
        const GuestAddr addr = m_rptrWriteback.load(std::memory_order_acquire);
        if (addr == 0 || !GuestMemory::Valid(addr, 4)) return;
        std::atomic_thread_fence(std::memory_order_release);
        m_memory.StoreBe32(addr, m_readIndex.load(std::memory_order_acquire));
    }

    void CommandProcessor::LogOnce(std::set<uint32_t>& seen, uint32_t key, const char* what, uint32_t detail) {
        if (!seen.insert(key).second) return;
        std::cerr << "[GPU] " << what << " 0x" << std::hex << detail << std::dec << std::endl;
    }
}
```

- [ ] **Step 5: Rodar e ver passar**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: `43 testes, 0 falharam`. As linhas `[GPU] pacote PM4 invalido ...`, `[GPU] opcode PM4 sem suporte ...` e `[GPU] WAIT_REG_MEM em registrador nunca satisfeito ...` são esperadas (testes de erro). Rodar 3 vezes para checar que os testes com threads são estáveis.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/gpu/command_processor.h src/gpu/command_processor.cpp tests/gpu/cp_rig.h tests/gpu/test_cp_ring.cpp tests/gpu/test_cp_registers.cpp tests/gpu/test_cp_flow.cpp
git commit -m "Adiciona command processor PM4 (ring, registradores, memoria, IB, WAIT, swap)" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Interrupções gráficas e vblank

**Files:**
- Create: `src/gpu/interrupts.h`, `src/gpu/interrupts.cpp`, `tests/gpu/test_interrupts.cpp`

**Interfaces:**
- Consumes: `Gpu::IInterruptSink` (Tarefa 4), `GuestAddr`
- Produces:
  - `Gpu::GuestCaller = std::function<void(GuestAddr fn, uint32_t arg0, uint32_t arg1)>`
  - `Gpu::InterruptDispatcher(GuestCaller)` com `SetCallback(GuestAddr, uint32_t)`, `SetEnabled(bool)`, `Dispatch(uint32_t source, uint32_t cpu)`, `StartVblank(std::function<void()> onTick, uint32_t hz = 60)`, `StopVblank()`. Convenção do callback guest: `fn(source, userData)`; source 0 = vblank, 1 = pacote INTERRUPT.

- [ ] **Step 1: Escrever os testes**

`tests/gpu/test_interrupts.cpp`:
```cpp
#include "gpu/interrupts.h"
#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace Gpu;

namespace {
    struct FakeGuest {
        struct Call { GuestAddr fn; uint32_t arg0; uint32_t arg1; };

        GuestCaller Caller() {
            return [this](GuestAddr fn, uint32_t a0, uint32_t a1) {
                std::lock_guard<std::mutex> lock(mutex);
                calls.push_back({ fn, a0, a1 });
            };
        }
        size_t Count() {
            std::lock_guard<std::mutex> lock(mutex);
            return calls.size();
        }

        std::mutex mutex;
        std::vector<Call> calls;
    };
}

TEST(dispatcher_calls_guest_with_source_and_user_data) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 0xBEEF);
    dispatcher.Dispatch(1, 3);

    CHECK_EQ(guest.Count(), 1);
    CHECK_EQ(guest.calls[0].fn, 0x82001000);
    CHECK_EQ(guest.calls[0].arg0, 1);
    CHECK_EQ(guest.calls[0].arg1, 0xBEEF);
}

TEST(dispatcher_ignores_interrupts_until_a_callback_is_registered) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.Dispatch(0, 2);
    CHECK_EQ(guest.Count(), 0);
}

TEST(dispatcher_does_not_call_the_guest_when_disabled) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 1);
    dispatcher.SetEnabled(false);
    dispatcher.Dispatch(0, 2);
    CHECK_EQ(guest.Count(), 0);
}

TEST(vblank_thread_ticks_and_dispatches_source_zero) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.SetCallback(0x82001000, 7);

    std::atomic<uint32_t> ticks{0};
    dispatcher.StartVblank([&] { ticks.fetch_add(1); }, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    dispatcher.StopVblank();

    CHECK(ticks.load() >= 5);
    CHECK_EQ(guest.Count(), ticks.load());
    for (const FakeGuest::Call& call : guest.calls) CHECK_EQ(call.arg0, 0);
}

TEST(vblank_ticks_even_without_a_registered_callback) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());

    std::atomic<uint32_t> ticks{0};
    dispatcher.StartVblank([&] { ticks.fetch_add(1); }, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dispatcher.StopVblank();

    CHECK(ticks.load() >= 3);
    CHECK_EQ(guest.Count(), 0);
}

TEST(vblank_stop_returns_promptly) {
    FakeGuest guest;
    InterruptDispatcher dispatcher(guest.Caller());
    dispatcher.StartVblank([] {}, 1);  // periodo de 1 s

    const auto start = std::chrono::steady_clock::now();
    dispatcher.StopVblank();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(500));
}
```

- [ ] **Step 2: Rodar e ver falhar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL de compilação, `Cannot open include file: 'gpu/interrupts.h'`.

- [ ] **Step 3: Implementar**

`src/gpu/interrupts.h`:
```cpp
#pragma once

#include "gpu/command_processor.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace Gpu {
    // Chama a funcao guest `fn(arg0, arg1)` numa thread/contexto guest. Em producao e o
    // Kernel::CreateGuestCaller; nos testes e um fake que so registra a chamada.
    using GuestCaller = std::function<void(GuestAddr fn, uint32_t arg0, uint32_t arg1)>;

    // Despacha o callback registrado por VdSetGraphicsInterruptCallback.
    //   source 0 = vblank, source 1 = pacote PM4 INTERRUPT.
    class InterruptDispatcher final : public IInterruptSink {
    public:
        explicit InterruptDispatcher(GuestCaller caller);
        ~InterruptDispatcher() override;

        void SetCallback(GuestAddr fn, uint32_t userData);
        // Desligado, Dispatch nao chama o guest (flag --no-gpu-interrupts); o vblank continua contando.
        void SetEnabled(bool enabled) { m_enabled.store(enabled); }

        // cpu e informativo: o guest recebe (source, userData), como no Xbox 360.
        void Dispatch(uint32_t source, uint32_t cpu) override;

        // Thread de vblank: a cada 1/hz s chama onTick() e despacha source 0.
        void StartVblank(std::function<void()> onTick, uint32_t hz = 60);
        void StopVblank();

    private:
        GuestCaller m_caller;
        std::atomic<bool> m_enabled{true};

        std::mutex m_callbackMutex;
        GuestAddr m_callback = 0;
        uint32_t m_userData = 0;

        std::mutex m_callMutex;  // o contexto guest do callback nao e reentrante

        std::mutex m_stopMutex;
        std::condition_variable m_stopCv;
        bool m_stopRequested = false;
        std::thread m_vblankThread;
    };
}
```

`src/gpu/interrupts.cpp`:
```cpp
#include "gpu/interrupts.h"

#include <chrono>

namespace Gpu {
    InterruptDispatcher::InterruptDispatcher(GuestCaller caller) : m_caller(std::move(caller)) {}

    InterruptDispatcher::~InterruptDispatcher() { StopVblank(); }

    void InterruptDispatcher::SetCallback(GuestAddr fn, uint32_t userData) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = fn;
        m_userData = userData;
    }

    void InterruptDispatcher::Dispatch(uint32_t source, uint32_t /*cpu*/) {
        if (!m_enabled.load() || !m_caller) return;

        GuestAddr fn;
        uint32_t userData;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            fn = m_callback;
            userData = m_userData;
        }
        if (fn == 0) return;  // so depois de registrado

        std::lock_guard<std::mutex> lock(m_callMutex);
        m_caller(fn, source, userData);
    }

    void InterruptDispatcher::StartVblank(std::function<void()> onTick, uint32_t hz) {
        if (m_vblankThread.joinable() || hz == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopRequested = false;
        }
        const auto period = std::chrono::nanoseconds(1000000000ull / hz);
        m_vblankThread = std::thread([this, onTick = std::move(onTick), period] {
            auto next = std::chrono::steady_clock::now();
            while (true) {
                next += period;
                {
                    std::unique_lock<std::mutex> lock(m_stopMutex);
                    if (m_stopCv.wait_until(lock, next, [this] { return m_stopRequested; })) return;
                }
                if (onTick) onTick();
                Dispatch(0, 2);
            }
        });
    }

    void InterruptDispatcher::StopVblank() {
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopRequested = true;
        }
        m_stopCv.notify_all();
        if (m_vblankThread.joinable()) m_vblankThread.join();
    }
}
```

- [ ] **Step 4: Rodar e ver passar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: `49 testes, 0 falharam`.

- [ ] **Step 5: Commit**

```powershell
git add src/gpu/interrupts.h src/gpu/interrupts.cpp tests/gpu/test_interrupts.cpp
git commit -m "Adiciona InterruptDispatcher com thread de vblank" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 6: GpuSystem e os 20 imports Vd*

**Files:**
- Create: `src/gpu/gpu_system.h`, `src/gpu/gpu_system.cpp`, `src/kernel/vd_exports.cpp`
- Create: `tests/gpu/vd_rig.h`, `tests/gpu/test_vd_video.cpp`, `tests/gpu/test_vd_swap.cpp`

**Interfaces:**
- Consumes: `CommandProcessor`, `InterruptDispatcher`, `GuestCaller`, `IGpuBackend`, `Gpu::Pm4::*`, `PPCContext` (`ppc_context.h`)
- Produces:
  - `Gpu::GpuSystemConfig { uint8_t* base; IGpuBackend* backend; std::function<GuestAddr(uint32_t)> allocate; GuestCaller caller; bool enableInterrupts = true; }`
  - `Gpu::GpuSystem::Get()` com `Initialize(const GpuSystemConfig&) -> bool`, `Shutdown()`, `IsInitialized()`, `StartEngines()`, `Memory()`, `Cp()`, `Interrupts()`, `Allocate(uint32_t)`, campo `gpuIdentifierAddress`
  - 20 funções `PPC_FUNC(__imp__Vd*)` (convenção: args em `r3..r10`, 9º argumento em diante em `r1 + 0x54 + 8*n`, retorno em `r3`)
  - `TestSupport`/`VdRig` (em `vd_rig.h`): inicializa `GpuSystem` sobre a memória de teste

- [ ] **Step 1: Escrever o rig e os testes**

`tests/gpu/vd_rig.h` (declara os `PPC_EXTERN_FUNC` dos 20 imports e o `VdRig`):
```cpp
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
```

`tests/gpu/test_vd_video.cpp`:
```cpp
#include "vd_rig.h"

// --- Modo de video, display e imports triviais ---------------------------------------------

TEST(vd_query_video_mode_writes_big_endian_struct) {
    VdRig rig;
    const GuestAddr p = Allocate(0x1000);
    rig.ctx.r3.u64 = p;
    __imp__VdQueryVideoMode(rig.ctx, rig.mem.Base());

    CHECK_EQ(rig.mem.LoadBe32(p + 0x00), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x04), 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x08), 0);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x0C), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x10), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x14), 0x42700000);  // 60.0f
    CHECK_EQ(rig.mem.LoadBe32(p + 0x18), 1);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x1C), 0x4A);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x20), 1);
}

TEST(vd_query_video_flags_reports_widescreen_hd) {
    VdRig rig;
    __imp__VdQueryVideoFlags(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 3);
}

TEST(vd_get_current_display_information_fills_scaler_and_display_fields) {
    VdRig rig;
    const GuestAddr p = Allocate(0x1000);
    rig.ctx.r3.u64 = p;
    __imp__VdGetCurrentDisplayInformation(rig.ctx, rig.mem.Base());

    const uint8_t* b = rig.mem.Base() + p;
    CHECK_EQ((b[0x00] << 8) | b[0x01], 1280);
    CHECK_EQ((b[0x02] << 8) | b[0x03], 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x10), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x14), 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x18), 1280);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x1C), 720);
    CHECK_EQ((b[0x40] << 8) | b[0x41], 320);
    CHECK_EQ((b[0x43] << 0) | (b[0x42] << 8), 180);
    CHECK_EQ((b[0x48] << 8) | b[0x49], 1280);
    CHECK_EQ((b[0x4A] << 8) | b[0x4B], 720);
    CHECK_EQ(rig.mem.LoadBe32(p + 0x4C), 0x42700000);
    CHECK_EQ((b[0x56] << 8) | b[0x57], 1280);
}

TEST(vd_get_current_display_gamma_returns_bt709) {
    VdRig rig;
    const GuestAddr type = Allocate(0x1000);
    rig.ctx.r3.u64 = type;
    rig.ctx.r4.u64 = type + 4;
    __imp__VdGetCurrentDisplayGamma(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(type), 2);
    CHECK_EQ(rig.mem.LoadBe32(type + 4), 0x400E38E4);  // 2.22222233f
}

TEST(vd_trivial_exports_return_expected_values) {
    VdRig rig;
    uint8_t* base = rig.mem.Base();

    __imp__VdInitializeEngines(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 1);
    __imp__VdIsHSIOTrainingSucceeded(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 1);
    __imp__VdSetDisplayMode(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdRetrainEDRAM(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdRetrainEDRAMWorker(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdEnableDisableClockGating(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdCallGraphicsNotificationRoutines(rig.ctx, base);
    CHECK_EQ(rig.ctx.r3.u32, 0);
    __imp__VdShutdownEngines(rig.ctx, base);  // sem efeito observavel; nao pode quebrar
}

TEST(vd_persist_display_allocates_and_returns_its_pointer) {
    VdRig rig;
    const GuestAddr out = Allocate(0x1000);
    rig.ctx.r3.u64 = 0;
    rig.ctx.r4.u64 = out;
    __imp__VdPersistDisplay(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 1);
    CHECK(rig.mem.LoadBe32(out) != 0);
}

TEST(vd_get_system_command_buffer_writes_markers) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    const GuestAddr other = Allocate(0x1000);
    rig.mem.StoreBe32(buffer + 8, 0xFFFFFFFF);
    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = other;
    __imp__VdGetSystemCommandBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(buffer), 0xBEEF0000);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 8), 0);  // zerado
    CHECK_EQ(rig.mem.LoadBe32(other), 0xBEEF0001);
}

TEST(vd_initialize_scaler_command_buffer_fills_with_nops) {
    VdRig rig;
    const GuestAddr dest = Allocate(0x1000);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54 + 2 * 8, dest);  // dest_ptr
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54 + 3 * 8, 4);     // dest_count
    __imp__VdInitializeScalerCommandBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.ctx.r3.u32, 4);
    CHECK_EQ(rig.mem.LoadBe32(dest), 0x80000000);
    CHECK_EQ(rig.mem.LoadBe32(dest + 12), 0x80000000);
    CHECK_EQ(rig.mem.LoadBe32(dest + 16), 0);
}

TEST(vd_set_system_command_buffer_gpu_identifier_address_is_stored) {
    VdRig rig;
    rig.ctx.r3.u64 = 0x40123450;
    __imp__VdSetSystemCommandBufferGpuIdentifierAddress(rig.ctx, rig.mem.Base());
    CHECK_EQ(GpuSystem::Get().gpuIdentifierAddress, 0x40123450);
}
```

`tests/gpu/test_vd_swap.cpp`:
```cpp
#include "vd_rig.h"

// --- Ring buffer, swap e interrupcoes -------------------------------------------

TEST(vd_swap_emits_fetch_constant_and_xe_swap_packet) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    const GuestAddr fetch = Allocate(0x1000);
    const GuestAddr frontPtr = Allocate(0x1000);
    const GuestAddr widthPtr = frontPtr + 4;
    const GuestAddr heightPtr = frontPtr + 8;
    const GuestAddr front = 0x42345000;

    const uint32_t fetchDwords[6] = { 0x00000002, front | 0x6, 0x0059F4FF, 0x1111, 0x2222, 0x3333 };
    for (uint32_t i = 0; i < 6; i++) rig.mem.StoreBe32(fetch + i * 4, fetchDwords[i]);
    rig.mem.StoreBe32(frontPtr, front);
    rig.mem.StoreBe32(widthPtr, 1280);
    rig.mem.StoreBe32(heightPtr, 720);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x54, widthPtr);
    rig.mem.StoreBe32(rig.ctx.r1.u32 + 0x5C, heightPtr);

    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = fetch;
    rig.ctx.r8.u64 = frontPtr;
    __imp__VdSwap(rig.ctx, rig.mem.Base());

    CHECK_EQ(rig.mem.LoadBe32(buffer + 0), Pm4::MakeType0(Reg::kShaderConstantFetch00_0, 6));
    for (uint32_t i = 0; i < 6; i++) CHECK_EQ(rig.mem.LoadBe32(buffer + 4 + i * 4), fetchDwords[i]);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 28), Pm4::MakeType3(Pm4::kXeSwap, 4));
    CHECK_EQ(rig.mem.LoadBe32(buffer + 32), Pm4::kSwapSignature);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 36), front);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 40), 1280);
    CHECK_EQ(rig.mem.LoadBe32(buffer + 44), 720);
    for (uint32_t i = 12; i < 64; i++) CHECK_EQ(rig.mem.LoadBe32(buffer + i * 4), 0x80000000);
}

TEST(vd_swap_with_invalid_arguments_leaves_the_buffer_untouched) {
    VdRig rig;
    const GuestAddr buffer = Allocate(0x1000);
    rig.ctx.r3.u64 = buffer;
    rig.ctx.r4.u64 = Allocate(0x1000);
    rig.ctx.r8.u64 = 0;  // ponteiro do frontbuffer nulo
    __imp__VdSwap(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.mem.LoadBe32(buffer), 0);
}

TEST(vd_set_graphics_interrupt_callback_registers_the_guest_function) {
    VdRig rig(true);
    rig.ctx.r3.u64 = 0x82005000;
    rig.ctx.r4.u64 = 0x1234;
    __imp__VdSetGraphicsInterruptCallback(rig.ctx, rig.mem.Base());

    GpuSystem::Get().Interrupts().Dispatch(1, 0);
    CHECK_EQ(rig.calls.size(), 1);
    CHECK_EQ(rig.calls[0].fn, 0x82005000);
    CHECK_EQ(rig.calls[0].a0, 1);
    CHECK_EQ(rig.calls[0].a1, 0x1234);
}

TEST(vd_initialize_ring_buffer_with_invalid_size_does_not_crash) {
    VdRig rig;
    rig.ctx.r3.u64 = Allocate(0x1000);
    rig.ctx.r4.u64 = 99;  // size_log2 absurdo
    __imp__VdInitializeRingBuffer(rig.ctx, rig.mem.Base());
    CHECK_EQ(rig.backend.swapCount.load(), 0);
}
```

- [ ] **Step 2: Rodar e ver falhar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL de compilação, `Cannot open include file: 'gpu/gpu_system.h'`.

- [ ] **Step 3: Implementar `GpuSystem`**

`src/gpu/gpu_system.h`:
```cpp
#pragma once

#include "gpu/command_processor.h"
#include "gpu/gpu_backend.h"
#include "gpu/guest_memory.h"
#include "gpu/interrupts.h"

#include <atomic>
#include <functional>
#include <memory>

namespace Gpu {
    struct GpuSystemConfig {
        uint8_t* base = nullptr;                          // base do espaco de 4 GB do guest
        IGpuBackend* backend = nullptr;                   // destino dos swaps
        std::function<GuestAddr(uint32_t)> allocate;      // aloca memoria do guest (VdPersistDisplay etc.)
        GuestCaller caller;                               // executa callbacks guest (interrupcoes)
        bool enableInterrupts = true;
    };

    // Estado global da GPU emulada: os imports Vd* sao PPC_FUNC livres, entao acessam isto via Get().
    class GpuSystem {
    public:
        static GpuSystem& Get();

        // Exige a pagina MMIO (Reg::kMmioBase, Reg::kMmioSize) commitada em `base`.
        bool Initialize(const GpuSystemConfig& config);
        void Shutdown();
        bool IsInitialized() const { return m_initialized.load(); }

        // Inicia a thread do CP e a de vblank (idempotente). Chamado por VdInitializeRingBuffer.
        void StartEngines();

        const GuestMemory& Memory() const { return m_memory; }
        CommandProcessor& Cp() { return *m_cp; }
        InterruptDispatcher& Interrupts() { return *m_interrupts; }
        GuestAddr Allocate(uint32_t size) const { return m_allocate ? m_allocate(size) : 0; }

        // VdSetSystemCommandBufferGpuIdentifierAddress: guardado, sem uso no M1 (o Xenia o ignora;
        // os fences chegam por EVENT_WRITE_SHD no stream de comandos).
        GuestAddr gpuIdentifierAddress = 0;

    private:
        GpuSystem() = default;

        std::atomic<bool> m_initialized{false};
        bool m_started = false;
        GuestMemory m_memory;
        std::function<GuestAddr(uint32_t)> m_allocate;
        std::unique_ptr<InterruptDispatcher> m_interrupts;
        std::unique_ptr<CommandProcessor> m_cp;
    };
}
```

`src/gpu/gpu_system.cpp`:
```cpp
#include "gpu/gpu_system.h"

#include "gpu/gpu_registers.h"

#include <iostream>

namespace Gpu {
    namespace {
        struct MmioDefault {
            uint32_t reg;
            uint32_t value;
        };

        // Valores que o D3D le da pagina MMIO (mesmos que o Xenia devolve nas leituras).
        constexpr MmioDefault kMmioDefaults[] = {
            { Reg::kCpRbWptr, 0 },
            { Reg::kRbEdramTiming, 0x08100748 },
            { Reg::kRbBcControl, 0x0000200E },
            { Reg::kD1ModeVCounter, 0x000002D0 },
            { Reg::kInterruptStatus, 0x00000001 },       // vblank
            { Reg::kD1ModeViewportSize, 0x050002D0 },    // 1280x720
        };
    }

    GpuSystem& GpuSystem::Get() {
        static GpuSystem instance;
        return instance;
    }

    bool GpuSystem::Initialize(const GpuSystemConfig& config) {
        if (m_initialized.load()) Shutdown();
        if (!config.base || !config.backend) {
            std::cerr << "[GPU] GpuSystem::Initialize sem base ou backend." << std::endl;
            return false;
        }

        m_memory = GuestMemory(config.base);
        m_allocate = config.allocate;
        gpuIdentifierAddress = 0;

        for (const MmioDefault& d : kMmioDefaults) {
            m_memory.StoreBe32(Reg::MmioAddress(d.reg), d.value);
        }

        m_interrupts = std::make_unique<InterruptDispatcher>(config.caller);
        m_interrupts->SetEnabled(config.enableInterrupts);
        m_cp = std::make_unique<CommandProcessor>(config.base, config.backend, m_interrupts.get());

        m_started = false;
        m_initialized = true;
        return true;
    }

    void GpuSystem::StartEngines() {
        if (!m_initialized || m_started) return;
        m_started = true;
        CommandProcessor* cp = m_cp.get();
        m_cp->Start();
        m_interrupts->StartVblank([cp] { cp->IncrementCounter(); });
    }

    void GpuSystem::Shutdown() {
        // Primeiro marca como desligado: imports Vd* chamados por uma thread guest ainda viva passam
        // a retornar cedo em vez de usar o CP que esta sendo destruido.
        if (!m_initialized.exchange(false)) return;
        m_interrupts->StopVblank();
        m_cp->Stop();
        m_cp.reset();
        m_interrupts.reset();
        m_started = false;
    }
}
```

- [ ] **Step 4: Implementar os imports Vd***

`src/kernel/vd_exports.cpp` (o `GpuCore` já o inclui pelo glob `src/kernel/*_exports.cpp`):
```cpp
// Imports Vd* do xboxkrnl.exe: a interface entre a D3D do jogo e a GPU emulada.
// Cada funcao segue a convencao do codigo recompilado: argumentos em r3..r10 (mais a pilha em
// r1 + 0x54 + 8 * n para o 9o argumento em diante) e retorno em r3.

#include "recompiled/ppc_context.h"

#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/pm4.h"

#include <cstring>
#include <iostream>

namespace {
    using Gpu::GpuSystem;

    constexpr uint32_t kDisplayWidth = 1280;
    constexpr uint32_t kDisplayHeight = 720;
    constexpr float kRefreshRate = 60.0f;
    constexpr uint32_t kStackArgBase = 0x54;

    uint32_t FloatBits(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // n-esimo argumento passado pela pilha (0 = o 9o argumento).
    uint32_t StackArg(PPCContext& ctx, uint32_t n) {
        return GpuSystem::Get().Memory().LoadBe32(ctx.r1.u32 + kStackArgBase + n * 8);
    }

    bool Ready(const char* name) {
        if (GpuSystem::Get().IsInitialized()) return true;
        std::cerr << "[Vd] " << name << " chamado antes de GpuSystem::Initialize." << std::endl;
        return false;
    }

    void Zero(GuestAddr addr, uint32_t bytes) {
        std::memset(GpuSystem::Get().Memory().Base() + addr, 0, bytes);
    }

    // struct X_VIDEO_MODE (48 bytes, big-endian), valores iguais aos do Xenia.
    void WriteVideoMode(GuestAddr addr) {
        const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
        Zero(addr, 48);
        mem.StoreBe32(addr + 0x00, kDisplayWidth);
        mem.StoreBe32(addr + 0x04, kDisplayHeight);
        mem.StoreBe32(addr + 0x08, 0);                       // is_interlaced
        mem.StoreBe32(addr + 0x0C, 1);                       // is_widescreen
        mem.StoreBe32(addr + 0x10, 1);                       // is_hi_def
        mem.StoreBe32(addr + 0x14, FloatBits(kRefreshRate)); // refresh_rate
        mem.StoreBe32(addr + 0x18, 1);                       // video_standard (NTSC)
        mem.StoreBe32(addr + 0x1C, 0x4A);                    // unknown_0x8a
        mem.StoreBe32(addr + 0x20, 0x01);                    // unknown_0x01
    }
}

// --- Inicializacao do ring buffer e da GPU ---------------------------------------------------

// r3 = endereco do ring (resultado de MmGetPhysicalAddress), r4 = log2 do tamanho
PPC_FUNC(__imp__VdInitializeRingBuffer) {
    if (!Ready("VdInitializeRingBuffer")) return;
    GpuSystem& gpu = GpuSystem::Get();
    gpu.Cp().InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
    gpu.StartEngines();
}

// r3 = endereco do write-back do RPTR, r4 = log2 do bloco
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    if (!Ready("VdEnableRingBufferRPtrWriteBack")) return;
    GpuSystem::Get().Cp().EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// r3 = callback guest (source, user_data), r4 = user_data
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    if (!Ready("VdSetGraphicsInterruptCallback")) return;
    GpuSystem::Get().Interrupts().SetCallback(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    if (!Ready("VdSetSystemCommandBufferGpuIdentifierAddress")) return;
    GpuSystem::Get().gpuIdentifierAddress = ctx.r3.u32;
}

PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdShutdownEngines) { (void)ctx; }
PPC_FUNC(__imp__VdRetrainEDRAM) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// --- Modo de video e display ----------------------------------------------------------------

// r3 = ponteiro para X_VIDEO_MODE
PPC_FUNC(__imp__VdQueryVideoMode) {
    if (!Ready("VdQueryVideoMode")) return;
    WriteVideoMode(ctx.r3.u32);
}

PPC_FUNC(__imp__VdQueryVideoFlags) {
    // bit0 = widescreen, bit1 = largura >= 1024, bit2 = largura >= 1920
    ctx.r3.u64 = 1u | (kDisplayWidth >= 1024 ? 2u : 0u) | (kDisplayWidth >= 1920 ? 4u : 0u);
}

PPC_FUNC(__imp__VdSetDisplayMode) { ctx.r3.u64 = 0; }

// r3 = ponteiro para X_DISPLAY_INFO (0x58 bytes, big-endian)
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    if (!Ready("VdGetCurrentDisplayInformation")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    const GuestAddr p = ctx.r3.u32;
    Zero(p, 0x58);
    mem.StoreBe16(p + 0x00, kDisplayWidth);   // front_buffer_width
    mem.StoreBe16(p + 0x02, kDisplayHeight);  // front_buffer_height
    mem.StoreBe32(p + 0x10, kDisplayWidth);   // scaler_source_rect.x2
    mem.StoreBe32(p + 0x14, kDisplayHeight);  // scaler_source_rect.y2
    mem.StoreBe32(p + 0x18, kDisplayWidth);   // scaled_output_width
    mem.StoreBe32(p + 0x1C, kDisplayHeight);  // scaled_output_height
    mem.StoreBe32(p + 0x20, 1);               // vertical_filter_type
    mem.StoreBe32(p + 0x30, 1);               // horizontal_filter_type
    mem.StoreBe16(p + 0x40, 320);             // overscan left
    mem.StoreBe16(p + 0x42, 180);             // overscan top
    mem.StoreBe16(p + 0x44, 320);             // overscan right
    mem.StoreBe16(p + 0x46, 180);             // overscan bottom
    mem.StoreBe16(p + 0x48, kDisplayWidth);   // display_width
    mem.StoreBe16(p + 0x4A, kDisplayHeight);  // display_height
    mem.StoreBe32(p + 0x4C, FloatBits(kRefreshRate));
    mem.StoreBe16(p + 0x56, kDisplayWidth);   // actual_display_width
}

// r3 = ponteiro para o tipo (2 = BT.709), r4 = ponteiro para o expoente (float)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    if (!Ready("VdGetCurrentDisplayGamma")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    mem.StoreBe32(ctx.r3.u32, 2);
    mem.StoreBe32(ctx.r4.u32, FloatBits(2.22222233f));
}

// r3 = unk, r4 = ponteiro de saida: recebe um buffer de 64 bytes do guest. Retorna 1.
PPC_FUNC(__imp__VdPersistDisplay) {
    if (!Ready("VdPersistDisplay")) return;
    GpuSystem& gpu = GpuSystem::Get();
    if (ctx.r4.u32 != 0) {
        const GuestAddr buffer = gpu.Allocate(64);
        if (buffer != 0) Zero(buffer, 64);
        gpu.Memory().StoreBe32(ctx.r4.u32, buffer);
    }
    ctx.r3.u64 = 1;
}

// r3 = buffer de 0x94 bytes (zerado, primeiro dword = 0xBEEF0000), r4 = dword (0xBEEF0001)
PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    if (!Ready("VdGetSystemCommandBuffer")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    Zero(ctx.r3.u32, 0x94);
    mem.StoreBe32(ctx.r3.u32, 0xBEEF0000);
    mem.StoreBe32(ctx.r4.u32, 0xBEEF0001);
}

// Argumentos: r3..r10 = scaler_source_xy, scaler_source_wh, scaled_output_xy, scaled_output_wh,
// front_buffer_wh, vertical_filter_type, vertical_filter_params, horizontal_filter_type;
// pilha: horizontal_filter_params, unk9, dest_ptr, dest_count. Preenche dest com NOPs (tipo 2)
// e retorna dest_count.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    if (!Ready("VdInitializeScalerCommandBuffer")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();
    const GuestAddr dest = StackArg(ctx, 2);
    const uint32_t count = StackArg(ctx, 3);
    if (dest != 0 && Gpu::GuestMemory::Valid(dest, uint64_t(count) * 4)) {
        for (uint32_t i = 0; i < count; i++) mem.StoreBe32(dest + i * 4, 0x80000000u);
    }
    ctx.r3.u64 = count;
}

// --- Swap -----------------------------------------------------------------------------------

// r3 = buffer de 64 dwords reservado no ring pelo D3D, r4 = fetch constant (6 dwords) do frontbuffer,
// r8 = ponteiro do endereco do frontbuffer; pilha: [0] = ponteiro de largura, [1] = ponteiro de altura.
// Escreve no buffer: fetch constant (tipo 0) + pacote XE_SWAP + NOPs. O CP executa o swap.
PPC_FUNC(__imp__VdSwap) {
    if (!Ready("VdSwap")) return;
    const Gpu::GuestMemory& mem = GpuSystem::Get().Memory();

    const GuestAddr buffer = ctx.r3.u32;
    const GuestAddr fetch = ctx.r4.u32;
    const GuestAddr frontbufferPtr = ctx.r8.u32;
    const GuestAddr widthPtr = StackArg(ctx, 0);
    const GuestAddr heightPtr = StackArg(ctx, 1);
    constexpr uint32_t kBufferDwords = 64;

    if (!Gpu::GuestMemory::Valid(buffer, kBufferDwords * 4) || !Gpu::GuestMemory::Valid(fetch, 24) ||
        frontbufferPtr == 0 || widthPtr == 0 || heightPtr == 0) {
        std::cerr << "[Vd] VdSwap com argumentos invalidos." << std::endl;
        return;
    }

    uint32_t fetchDwords[6];
    for (uint32_t i = 0; i < 6; i++) fetchDwords[i] = mem.LoadBe32(fetch + i * 4);

    // A fetch constant do D3D guarda o endereco virtual do frontbuffer em dword_1[31:12]. Como
    // MmGetPhysicalAddress e a identidade neste port, ele ja e o endereco que o CP usa.
    const uint32_t frontbuffer = (fetchDwords[1] >> 12) << 12;
    const uint32_t width = mem.LoadBe32(widthPtr);
    const uint32_t height = mem.LoadBe32(heightPtr);

    uint32_t offset = 0;
    auto emit = [&](uint32_t dword) { mem.StoreBe32(buffer + (offset++) * 4, dword); };

    emit(Gpu::Pm4::MakeType0(Gpu::Reg::kShaderConstantFetch00_0, 6));
    for (uint32_t dword : fetchDwords) emit(dword);

    emit(Gpu::Pm4::MakeType3(Gpu::Pm4::kXeSwap, 4));
    emit(Gpu::Pm4::kSwapSignature);
    emit(frontbuffer);
    emit(width);
    emit(height);

    while (offset < kBufferDwords) emit(Gpu::Pm4::MakeType2());
}
```

- [ ] **Step 5: Rodar e ver passar**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: `62 testes, 0 falharam`. (O `cmake -S . -B build` é necessário para o glob enxergar o arquivo novo.)

- [ ] **Step 6: Commit**

```powershell
git add src/gpu/gpu_system.h src/gpu/gpu_system.cpp src/kernel/vd_exports.cpp tests/gpu/vd_rig.h tests/gpu/test_vd_video.cpp tests/gpu/test_vd_swap.cpp
git commit -m "Adiciona GpuSystem e os 20 imports Vd* com testes" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Selftest do D3D simulado (teste de integração)

**Files:**
- Create: `src/gpu/selftest.h`, `src/gpu/selftest.cpp`, `tests/gpu/test_selftest.cpp`

**Interfaces:**
- Consumes: `GpuSystem`, `__imp__VdInitializeRingBuffer`, `__imp__VdEnableRingBufferRPtrWriteBack`, `__imp__VdSwap`
- Produces: `bool Gpu::RunSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel = nullptr)`: devolve `true` se o write-back do RPTR alcançou o WPTR em até 2 s.

- [ ] **Step 1: Escrever o teste**

`tests/gpu/test_selftest.cpp`:
```cpp
#include "gpu/selftest.h"
#include "vd_rig.h"

TEST(selftest_drives_swaps_through_vd_exports_and_the_command_processor) {
    VdRig rig;
    CHECK(RunSelfTest(5, 1));
    CHECK_EQ(rig.backend.swapCount.load(), 5);
    CHECK_EQ(rig.backend.lastWidth, 1280);
    CHECK_EQ(rig.backend.lastHeight, 720);
    CHECK_EQ(GpuSystem::Get().Cp().Stats().malformed, 0);
}
```

- [ ] **Step 2: Rodar e ver falhar**

```powershell
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
```
Expected: FAIL de compilação, `Cannot open include file: 'gpu/selftest.h'`.

- [ ] **Step 3: Implementar**

`src/gpu/selftest.h`:
```cpp
#pragma once

#include <atomic>
#include <cstdint>

namespace Gpu {
    // Simula o D3D do jogo sem o jogo: inicializa o ring pelos imports Vd*, publica `frames`
    // swaps (VdSwap + WPTR) e espera o command processor consumi-los. Exige GpuSystem inicializado.
    // Retorna true se o write-back do RPTR alcancou o WPTR a tempo (2 s). `cancel`, se dado, interrompe
    // a publicacao de novos frames quando vira true (ex.: janela fechada).
    bool RunSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel = nullptr);
}
```

`src/gpu/selftest.cpp`:
```cpp
#include "gpu/selftest.h"

#include "recompiled/ppc_context.h"

#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

PPC_EXTERN_FUNC(__imp__VdInitializeRingBuffer);
PPC_EXTERN_FUNC(__imp__VdEnableRingBufferRPtrWriteBack);
PPC_EXTERN_FUNC(__imp__VdSwap);

namespace Gpu {
    bool RunSelfTest(uint32_t frames, uint32_t frameIntervalMs, const std::atomic<bool>* cancel) {
        GpuSystem& gpu = GpuSystem::Get();
        if (!gpu.IsInitialized()) return false;
        const GuestMemory& mem = gpu.Memory();
        uint8_t* base = mem.Base();

        constexpr uint32_t kRingSizeLog2 = 13;                      // 1 << (13 + 3) = 64 KB
        constexpr uint32_t kRingBytes = 1u << (kRingSizeLog2 + 3);
        constexpr uint32_t kRingDwords = kRingBytes / 4;
        constexpr uint32_t kSwapDwords = 64;                        // reserva de VdSwap no ring
        constexpr uint32_t kWidth = 1280;
        constexpr uint32_t kHeight = 720;

        // scratch: +0x000 write-back do RPTR, +0x010 fetch constant, +0x030 ptr do frontbuffer,
        //          +0x034 largura, +0x038 altura, +0x800 topo da pilha fake (r1)
        const GuestAddr ring = gpu.Allocate(kRingBytes);
        const GuestAddr scratch = gpu.Allocate(0x1000);
        const GuestAddr frontbuffer = gpu.Allocate(0x1000);
        if (!ring || !scratch || !frontbuffer) {
            std::cerr << "[SelfTest] Falha ao alocar memoria do guest." << std::endl;
            return false;
        }
        std::memset(base + ring, 0, kRingBytes);
        std::memset(base + scratch, 0, 0x1000);

        const GuestAddr writeback = scratch + 0x000;
        const GuestAddr fetch = scratch + 0x010;
        const GuestAddr frontbufferPtr = scratch + 0x030;
        const GuestAddr widthPtr = scratch + 0x034;
        const GuestAddr heightPtr = scratch + 0x038;
        const GuestAddr stack = scratch + 0x800;

        mem.StoreBe32(fetch + 4, frontbuffer);                        // dword_1: base_address << 12
        mem.StoreBe32(fetch + 8, (kWidth - 1) | ((kHeight - 1) << 13)); // dword_2: size_2d
        mem.StoreBe32(frontbufferPtr, frontbuffer);
        mem.StoreBe32(widthPtr, kWidth);
        mem.StoreBe32(heightPtr, kHeight);
        mem.StoreBe32(stack + 0x54, widthPtr);
        mem.StoreBe32(stack + 0x5C, heightPtr);

        PPCContext ctx{};
        ctx.r1.u64 = stack;

        ctx.r3.u64 = ring;
        ctx.r4.u64 = kRingSizeLog2;
        __imp__VdInitializeRingBuffer(ctx, base);

        ctx.r3.u64 = writeback;
        ctx.r4.u64 = 6;
        __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);

        uint32_t writeIndex = 0;
        for (uint32_t frame = 0; frame < frames; frame++) {
            if (cancel && cancel->load()) break;
            ctx.r3.u64 = ring + writeIndex * 4;
            ctx.r4.u64 = fetch;
            ctx.r8.u64 = frontbufferPtr;
            __imp__VdSwap(ctx, base);

            writeIndex = (writeIndex + kSwapDwords) % kRingDwords;
            mem.StoreBe32(Reg::MmioAddress(Reg::kCpRbWptr), writeIndex);

            std::this_thread::sleep_for(std::chrono::milliseconds(frameIntervalMs));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (mem.LoadBe32(writeback) != writeIndex) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::cerr << "[SelfTest] CP nao consumiu o ring a tempo." << std::endl;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
}
```

- [ ] **Step 4: Rodar e ver passar**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
ctest --test-dir build -C Release
```
Expected: `63 testes, 0 falharam` e `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Commit**

```powershell
git add src/gpu/selftest.h src/gpu/selftest.cpp tests/gpu/test_selftest.cpp
git commit -m "Adiciona selftest do D3D simulado como teste de integracao" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 8: `MmGetPhysicalAddress` real e stubs gerados para os demais imports

O código recompilado declara 219 `__imp__*` e nenhum tinha definição. Os 20 `Vd*` (Tarefa 6) e `MmGetPhysicalAddress` agora têm implementação real; os outros 198 ganham stubs que logam uma vez e retornam 0. Não usar weak: o `ppc_context.h` zera `__attribute__` no MSVC.

**Files:**
- Create: `src/kernel/mm_exports.cpp`, `tools/gen_import_stubs.py`, `src/kernel/import_stubs.cpp` (gerado)

**Interfaces:**
- Consumes: `src/recompiled/ppc_recomp_shared.h` (os `PPC_EXTERN_FUNC(__imp__...)`), `src/kernel/*_exports.cpp`
- Produces: definição de todos os 219 `__imp__*`; `python tools/gen_import_stubs.py [raiz]` regenera `import_stubs.cpp`

- [ ] **Step 1: Criar `mm_exports.cpp`**

`src/kernel/mm_exports.cpp`:
```cpp
// Imports Mm* do xboxkrnl.exe com implementacao real.

#include "recompiled/ppc_context.h"

// r3 = endereco virtual -> r3 = endereco fisico. Este port nao separa fisico de virtual: o ring
// buffer e os buffers que o command processor le ficam no mesmo espaco de enderecos do guest,
// entao a identidade basta.
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    ctx.r3.u64 = ctx.r3.u32;
}
```

- [ ] **Step 2: Criar o gerador**

`tools/gen_import_stubs.py`:
```python
#!/usr/bin/env python3
"""Gera src/kernel/import_stubs.cpp.

O codigo recompilado declara cada import do kernel/XAM como `PPC_EXTERN_FUNC(__imp__Nome)`
em src/recompiled/ppc_recomp_shared.h. Cada um precisa de uma definicao no host para o
executavel linkar. Este script emite um stub que loga (uma vez) e retorna 0 para todo import
que ainda NAO tem implementacao real em src/kernel/*_exports.cpp.

Para implementar um import de verdade: defina `PPC_FUNC(__imp__Nome)` num arquivo
src/kernel/*_exports.cpp e rode este script de novo; o stub correspondente some.

Uso: python tools/gen_import_stubs.py [raiz-do-projeto]
"""

import re
import sys
from pathlib import Path

IMPORT_RE = re.compile(r"PPC_EXTERN_FUNC\((__imp__\w+)\)")
IMPL_RE = re.compile(r"^PPC_FUNC\((__imp__\w+)\)", re.MULTILINE)

HEADER = """\
// GERADO por tools/gen_import_stubs.py -- nao edite a mao.
// Stubs de log para os imports do kernel/XAM sem implementacao real em src/kernel/*_exports.cpp.
// Para implementar um import, defina PPC_FUNC(__imp__Nome) num *_exports.cpp e rode o script.

#include "recompiled/ppc_context.h"

#include <atomic>
#include <cstdio>

#define UNIMPLEMENTED_IMPORT(name)                                                         \\
    PPC_FUNC(__imp__##name) {                                                              \\
        static std::atomic<bool> logged{false};                                            \\
        if (!logged.exchange(true)) {                                                      \\
            std::fprintf(stderr, "[Kernel] import nao implementado: %s\\n", #name);         \\
        }                                                                                  \\
        ctx.r3.u64 = 0;                                                                    \\
    }

"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    shared = root / "src" / "recompiled" / "ppc_recomp_shared.h"
    kernel_dir = root / "src" / "kernel"
    out = kernel_dir / "import_stubs.cpp"

    imported = list(dict.fromkeys(IMPORT_RE.findall(shared.read_text(encoding="utf-8"))))
    if not imported:
        print(f"erro: nenhum __imp__ encontrado em {shared}", file=sys.stderr)
        return 1

    implemented = set()
    for path in sorted(kernel_dir.glob("*_exports.cpp")):
        implemented.update(IMPL_RE.findall(path.read_text(encoding="utf-8")))

    unknown = sorted(implemented - set(imported))
    if unknown:
        print("erro: PPC_FUNC definido para import que o jogo nao usa (typo?): " + ", ".join(unknown),
              file=sys.stderr)
        return 1

    stubs = [name for name in imported if name not in implemented]
    lines = [f"UNIMPLEMENTED_IMPORT({name[len('__imp__'):]})" for name in stubs]
    out.write_text(HEADER + "\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    print(f"{len(imported)} imports: {len(implemented)} implementados, {len(stubs)} stubs -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Gerar e conferir**

```powershell
python tools/gen_import_stubs.py
```
Expected: `219 imports: 21 implementados, 198 stubs -> ...\src\kernel\import_stubs.cpp`

```powershell
Select-String -Path src/kernel/import_stubs.cpp -Pattern "UNIMPLEMENTED_IMPORT\(" | Measure-Object | Select-Object -ExpandProperty Count
Select-String -Path src/kernel/import_stubs.cpp -Pattern "Vd|MmGetPhysicalAddress"
```
Expected: `199` (198 stubs + a linha do `#define`) e nenhuma linha em `Select-String` (os imports reais não geram stub).

- [ ] **Step 4: Verificar a cobertura dos símbolos**

```powershell
python -c "import re,glob;d=set();[d.update(re.findall(r'^(?:PPC_FUNC_IMPL|PPC_WEAK_FUNC|PPC_FUNC)\((\w+)\)',open(f,encoding='utf-8',errors='ignore').read(),re.M)) for f in glob.glob('src/recompiled/ppc_recomp.*.cpp')];m=set(re.findall(r'\{\s*0x[0-9A-Fa-f]+,\s*(\w+)\s*\}',open('src/recompiled/ppc_func_mapping.cpp',encoding='utf-8').read()));print('faltando:',sorted(n for n in m if n not in d and not n.startswith('__imp__')))"
```
Expected: `faltando: []` (tudo que o mapeamento referencia é definido no código recompilado ou é um import).

- [ ] **Step 5: Commit**

```powershell
git add src/kernel/mm_exports.cpp tools/gen_import_stubs.py src/kernel/import_stubs.cpp
git commit -m "Adiciona MmGetPhysicalAddress real e stubs gerados para os imports restantes" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Integração com o renderer Plume, `main`, memória e `gpu_selftest`

Troca o loop `BeginFrame`/`EndFrame` da thread principal por `PresentFrame`, chamado pelo CP a cada `XE_SWAP`. A thread principal passa a cuidar só da janela.

**Files:**
- Modify: `src/graphics/renderer.h`, `src/graphics/renderer.cpp`, `src/main.cpp`, `src/kernel/memory.cpp`, `CMakeLists.txt`
- Create: `src/kernel/guest_call.h`, `src/kernel/guest_call.cpp`, `src/gpu_selftest/main.cpp`

**Interfaces:**
- Consumes: `Gpu::IGpuBackend`, `Gpu::GpuSystem`, `Gpu::GpuSystemConfig`, `Gpu::RunSelfTest`, `Gpu::Reg::kMmioBase/kMmioSize`, `Kernel::MemoryManager::AllocateVirtual`
- Produces:
  - `Graphics::NativeRenderer::PresentFrame()`, `NotifyResize()`; `Graphics::RendererBackend : Gpu::IGpuBackend`
  - `Kernel::CreateGuestCaller(uint8_t* base, std::function<GuestAddr(uint32_t)> allocate) -> Kernel::GuestCallerFn`
  - Executável `gpu_selftest [frames]` em `build\bin\Release\`

- [ ] **Step 1: Substituir `src/graphics/renderer.h`**

Mudanças: `s_running` vira `std::atomic<bool>`; `BeginFrame`/`EndFrame` saem; entram `PresentFrame`/`NotifyResize` e a classe `RendererBackend`.
```cpp
#pragma once

#include <atomic>
#include <string>
#include <memory>
#include <cstdint>

#include "gpu/gpu_backend.h"

namespace Graphics {
    enum class Backend {
        Direct3D12,
        Vulkan
    };

    struct RenderConfig {
        uint32_t width = 1280;
        uint32_t height = 720;
        bool vsync = true;
        bool fullscreen = false;
        Backend backend = Backend::Vulkan;
        // Se o backend pedido falhar ao inicializar, tenta o outro automaticamente.
        bool allowBackendFallback = true;
        // Cor de limpeza do framebuffer (RGBA).
        float clearColor[4] = { 0.0f, 0.18f, 0.05f, 1.0f };
    };

    // Janela + device Plume (Vulkan ou D3D12).
    // Threads: Initialize/Shutdown/PollEvents/IsRunning rodam na thread principal (dona da janela);
    // PresentFrame roda na thread do command processor. Depois de Initialize, so a thread do CP
    // toca no device ate o Shutdown (que acontece depois do join do CP).
    class NativeRenderer {
    public:
        static bool Initialize(const RenderConfig& config);
        static void Shutdown();

        // Adquire a imagem da swapchain, limpa, submete na fila e apresenta (vkQueuePresentKHR / Present).
        // Chamado pelo command processor a cada pacote XE_SWAP.
        static void PresentFrame();
        // Marca a swapchain para recriacao no proximo PresentFrame. Thread-safe.
        static void NotifyResize();

        static bool IsRunning();
        static void PollEvents();

        static uint32_t GetWidth() { return s_config.width; }
        static uint32_t GetHeight() { return s_config.height; }
        static Backend GetActiveBackend() { return s_config.backend; }
        static const char* GetBackendName();

    private:
        static RenderConfig s_config;
        static bool s_initialized;
        static std::atomic<bool> s_running;
    };

    // Liga o command processor ao renderer: XE_SWAP -> PresentFrame.
    class RendererBackend final : public Gpu::IGpuBackend {
    public:
        void Swap(uint32_t frontbufferAddr, uint32_t width, uint32_t height) override;
        void OnResize(uint32_t width, uint32_t height) override;
    };
}
```

- [ ] **Step 2: Substituir `src/graphics/renderer.cpp`**

Mudanças em relação ao arquivo atual: inclui `<atomic>`; `s_running`, `s_resizePending` e `s_minimized` são atômicos (a thread da janela e a do CP os compartilham); `s_frameActive`/`s_imageIndex` somem; `BeginFrame`+`EndFrame` viram `PresentFrame` (acquire, clear, submit, present, espera do fence numa chamada só); `NotifyResize` e `RendererBackend` são novos. O restante (criação do device, swapchain, janela) é idêntico.
```cpp
#include "renderer.h"

#include <atomic>
#include <iostream>
#include <vector>

#include "plume_render_interface.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace plume {
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#if defined(_WIN32)
    extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}

namespace Graphics {
    RenderConfig NativeRenderer::s_config{};
    bool NativeRenderer::s_initialized = false;
    std::atomic<bool> NativeRenderer::s_running{false};

    namespace {
        constexpr uint32_t kBufferCount = 2;
        constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::B8G8R8A8_UNORM;

        // Todo o estado GPU. A ordem de declaracao define a ordem inversa de destruicao
        // (framebuffers/semaforos antes da swapchain, swapchain antes da fila, fila antes do device).
        struct GpuContext {
            std::unique_ptr<plume::RenderInterface> interface;
            std::unique_ptr<plume::RenderDevice> device;
            std::unique_ptr<plume::RenderCommandQueue> queue;
            std::unique_ptr<plume::RenderSwapChain> swapChain;
            std::unique_ptr<plume::RenderCommandList> commandList;
            std::unique_ptr<plume::RenderCommandFence> fence;
            std::unique_ptr<plume::RenderCommandSemaphore> acquireSemaphore;
            std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> releaseSemaphores;
            std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
        };

        std::unique_ptr<GpuContext> s_gpu;
        // Escritos pela thread da janela (WM_SIZE) e lidos pela thread do CP.
        std::atomic<bool> s_resizePending{false};
        std::atomic<bool> s_minimized{false};

#if defined(_WIN32)
        HWND s_hwnd = nullptr;

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            switch (msg) {
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                s_minimized = (wParam == SIZE_MINIMIZED);
                s_resizePending = true;
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    PostQuitMessage(0);
                }
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }
#endif

        const char* BackendToString(Backend backend) {
            return backend == Backend::Vulkan ? "Vulkan" : "Direct3D 12";
        }

        std::unique_ptr<plume::RenderInterface> CreateInterface(Backend backend) {
            switch (backend) {
            case Backend::Vulkan:
                return plume::CreateVulkanInterface();
            case Backend::Direct3D12:
#if defined(_WIN32)
                return plume::CreateD3D12Interface();
#else
                return nullptr;
#endif
            }
            return nullptr;
        }

        void CreateFramebuffers(GpuContext& gpu) {
            gpu.framebuffers.clear();
            for (uint32_t i = 0; i < gpu.swapChain->getTextureCount(); i++) {
                const plume::RenderTexture* color = gpu.swapChain->getTexture(i);

                plume::RenderFramebufferDesc desc;
                desc.colorAttachments = &color;
                desc.colorAttachmentsCount = 1;
                desc.depthAttachment = nullptr;
                gpu.framebuffers.push_back(gpu.device->createFramebuffer(desc));
            }

            while (gpu.releaseSemaphores.size() < gpu.swapChain->getTextureCount()) {
                gpu.releaseSemaphores.emplace_back(gpu.device->createCommandSemaphore());
            }
        }

        // Cria interface -> device -> fila -> swapchain para o backend pedido.
        bool CreateGpuContext(Backend backend, plume::RenderWindow window, bool vsync) {
            auto gpu = std::make_unique<GpuContext>();

            gpu->interface = CreateInterface(backend);
            if (!gpu->interface) {
                std::cerr << "[Renderer] " << BackendToString(backend)
                          << " indisponivel (driver/loader nao encontrado)." << std::endl;
                return false;
            }

            gpu->device = gpu->interface->createDevice();
            if (!gpu->device) {
                std::cerr << "[Renderer] Falha ao criar o dispositivo " << BackendToString(backend) << "." << std::endl;
                return false;
            }

            gpu->queue = gpu->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
            if (!gpu->queue) {
                std::cerr << "[Renderer] Falha ao criar a fila de comandos." << std::endl;
                return false;
            }

            gpu->swapChain = gpu->queue->createSwapChain(plume::RenderSwapChainDesc(window, kSwapchainFormat, kBufferCount));
            if (!gpu->swapChain) {
                std::cerr << "[Renderer] Falha ao criar a swapchain." << std::endl;
                return false;
            }
            gpu->swapChain->setVsyncEnabled(vsync);
            if (!gpu->swapChain->resize()) {
                std::cerr << "[Renderer] Falha ao dimensionar a swapchain." << std::endl;
                return false;
            }

            gpu->commandList = gpu->queue->createCommandList();
            gpu->fence = gpu->device->createCommandFence();
            gpu->acquireSemaphore = gpu->device->createCommandSemaphore();
            if (!gpu->commandList || !gpu->fence || !gpu->acquireSemaphore) {
                std::cerr << "[Renderer] Falha ao criar objetos de sincronizacao." << std::endl;
                return false;
            }

            CreateFramebuffers(*gpu);

            const plume::RenderDeviceDescription& desc = gpu->device->getDescription();
            std::cout << "[Renderer] GPU: " << desc.name << std::endl;

            s_gpu = std::move(gpu);
            return true;
        }

        bool ResizeSwapChain() {
            GpuContext& gpu = *s_gpu;
            // Libera referencias as imagens antigas antes de recriar a swapchain.
            gpu.framebuffers.clear();
            const bool ok = gpu.swapChain->resize();
            if (ok) {
                CreateFramebuffers(gpu);
            }
            return ok;
        }
    }

    bool NativeRenderer::Initialize(const RenderConfig& config) {
        s_config = config;
        std::cout << "====================================================\n";
        std::cout << "[Renderer] Inicializando Renderizador Nativo (" << BackendToString(s_config.backend) << ")...\n";
        std::cout << "[Renderer] Resolucao: " << s_config.width << "x" << s_config.height << "\n";
        std::cout << "====================================================\n";

#if defined(_WIN32)
        HINSTANCE hInstance = GetModuleHandle(nullptr);
        const char* className = "GreenLanternRecompWindowClass";

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className;

        RegisterClassExA(&wc);

        RECT rect = { 0, 0, static_cast<LONG>(s_config.width), static_cast<LONG>(s_config.height) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        s_hwnd = CreateWindowExA(
            0,
            className,
            "Green Lantern: Rise of the Manhunters - Native PC Port",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr, nullptr, hInstance, nullptr
        );

        if (!s_hwnd) {
            std::cerr << "[Renderer] Falha ao criar a janela Win32 nativa." << std::endl;
            return false;
        }

        ShowWindow(s_hwnd, SW_SHOW);
        UpdateWindow(s_hwnd);

        const plume::RenderWindow window = s_hwnd;
#else
        std::cerr << "[Renderer] Plataforma sem suporte a janela nativa." << std::endl;
        return false;
#endif

        Backend backends[2] = {
            s_config.backend,
            s_config.backend == Backend::Vulkan ? Backend::Direct3D12 : Backend::Vulkan
        };
        const int attempts = s_config.allowBackendFallback ? 2 : 1;

        bool created = false;
        for (int i = 0; i < attempts && !created; i++) {
            if (i > 0) {
                std::cout << "[Renderer] Tentando fallback para " << BackendToString(backends[i]) << "..." << std::endl;
            }
            created = CreateGpuContext(backends[i], window, s_config.vsync);
            if (created) {
                s_config.backend = backends[i];
            }
        }

        if (!created) {
#if defined(_WIN32)
            DestroyWindow(s_hwnd);
            s_hwnd = nullptr;
#endif
            return false;
        }

        s_resizePending = false;
        s_initialized = true;
        s_running = true;

        std::cout << "[Renderer] Janela e contexto grafico nativo (" << GetBackendName()
                  << ") criados com sucesso." << std::endl;
        return true;
    }

    void NativeRenderer::Shutdown() {
        if (!s_initialized) return;

        if (s_gpu) {
            // Cada PresentFrame espera o fence, logo a GPU ja esta ociosa aqui.
            s_gpu.reset();
        }

#if defined(_WIN32)
        if (s_hwnd) {
            DestroyWindow(s_hwnd);
            s_hwnd = nullptr;
        }
#endif
        s_initialized = false;
        s_running = false;
        std::cout << "[Renderer] Renderizador nativo finalizado." << std::endl;
    }

    void NativeRenderer::PollEvents() {
#if defined(_WIN32)
        MSG msg = {};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
#endif
    }

    bool NativeRenderer::IsRunning() {
        return s_running;
    }

    const char* NativeRenderer::GetBackendName() {
        return BackendToString(s_config.backend);
    }

    void NativeRenderer::NotifyResize() {
        s_resizePending = true;
    }

    void NativeRenderer::PresentFrame() {
        if (!s_initialized || !s_running || !s_gpu || s_minimized) {
            return;
        }

        GpuContext& gpu = *s_gpu;

        if (s_resizePending || gpu.swapChain->needsResize()) {
            s_resizePending = false;
            if (!ResizeSwapChain() || gpu.swapChain->isEmpty()) {
                return;
            }
        }

        uint32_t imageIndex = 0;
        if (!gpu.swapChain->acquireTexture(gpu.acquireSemaphore.get(), &imageIndex)) {
            // Swapchain desatualizada (VK_ERROR_OUT_OF_DATE_KHR): recria no proximo frame.
            s_resizePending = true;
            return;
        }

        gpu.commandList->begin();

        plume::RenderTexture* target = gpu.swapChain->getTexture(imageIndex);
        gpu.commandList->barriers(plume::RenderBarrierStage::GRAPHICS,
                                  plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
        gpu.commandList->setFramebuffer(gpu.framebuffers[imageIndex].get());

        const uint32_t width = gpu.swapChain->getWidth();
        const uint32_t height = gpu.swapChain->getHeight();
        gpu.commandList->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
        gpu.commandList->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));

        const float* c = s_config.clearColor;
        gpu.commandList->clearColor(0, plume::RenderColor(c[0], c[1], c[2], c[3]));

        gpu.commandList->barriers(plume::RenderBarrierStage::NONE,
                                  plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
        gpu.commandList->end();

        const plume::RenderCommandList* commandList = gpu.commandList.get();
        plume::RenderCommandSemaphore* waitSemaphore = gpu.acquireSemaphore.get();
        plume::RenderCommandSemaphore* signalSemaphore = gpu.releaseSemaphores[imageIndex].get();

        gpu.queue->executeCommandLists(&commandList, 1, &waitSemaphore, 1, &signalSemaphore, 1, gpu.fence.get());

        // Apresentacao da swapchain (vkQueuePresentKHR no backend Vulkan).
        if (!gpu.swapChain->present(imageIndex, &signalSemaphore, 1)) {
            s_resizePending = true;
        }
        gpu.queue->waitForCommandFence(gpu.fence.get());
    }

    void RendererBackend::Swap(uint32_t, uint32_t, uint32_t) {
        // O frontbuffer so passa a importar quando o renderer desenhar o jogo (sub-projeto 5).
        NativeRenderer::PresentFrame();
    }

    void RendererBackend::OnResize(uint32_t, uint32_t) {
        NativeRenderer::NotifyResize();
    }
}
```

- [ ] **Step 3: Criar `guest_call`**

`src/kernel/guest_call.h`:
```cpp
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
```

`src/kernel/guest_call.cpp`:
```cpp
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
```

- [ ] **Step 4: Editar `src/kernel/memory.cpp`**

(a) Nos includes, trocar
```cpp
#include "../recompiled/ppc_recomp_shared.h"
#include <algorithm>
```
por
```cpp
#include "../recompiled/ppc_recomp_shared.h"
#include "gpu/gpu_registers.h"
#include <algorithm>
#include <atomic>
```

(b) Antes de `if (!titleRegion || !dispatchRegion || !stackRegion || !heapRegion) {`, inserir o commit da página MMIO e incluí-la na checagem:
```cpp
        // 5. Commitar a pagina de registradores da GPU (MMIO do Xenos): o D3D escreve o WPTR do ring
        //    ali e o command processor faz polling dele (src/gpu/command_processor.cpp)
        void* gpuMmioRegion = VirtualAlloc(
            s_baseMemory + Gpu::Reg::kMmioBase,
            Gpu::Reg::kMmioSize,
            MEM_COMMIT,
            PAGE_READWRITE
        );

        if (!titleRegion || !dispatchRegion || !stackRegion || !heapRegion || !gpuMmioRegion) {
```

(c) Em `MemoryManager::AllocateVirtual`, trocar o corpo por uma versão atômica:
```cpp
        (void)protection;
        // Atomico: o guest, o command processor e a thread de interrupcoes alocam concorrentemente.
        static std::atomic<GuestAddr> s_heapCursor{0x40000000};
        // Alinhamento em 64KB (padrão de página do Xenon)
        size_t alignedSize = (size + 0xFFFF) & ~0xFFFF;
        return s_heapCursor.fetch_add(static_cast<GuestAddr>(alignedSize));
```

- [ ] **Step 5: Substituir `src/main.cpp`**

Mudanças: `--no-gpu-interrupts`; cria o `RendererBackend` e o `GpuSystem` depois do renderer; o loop principal só chama `PollEvents`; `GpuSystem::Shutdown()` roda antes de `NativeRenderer::Shutdown()` (o CP usa o device).
```cpp
#include <iostream>
#include "types.h"
#include "platform/filesystem.h"
#include "kernel/memory.h"
#include "kernel/kernel_stubs.h"
#include "kernel/guest_thread.h"
#include "kernel/guest_call.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include "graphics/renderer.h"
#include "gpu/gpu_system.h"

int main(int argc, char** argv) {
    // --no-gpu-interrupts: nao dispara o callback guest de vblank/INTERRUPT da GPU.
    bool gpuInterrupts = true;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-gpu-interrupts") == 0) gpuInterrupts = false;
    }

    std::cout << "====================================================\n";
    std::cout << " Green Lantern: Rise of the Manhunters - PC Recomp \n";
    std::cout << " Native Recompilation Engine (Vulkan / Direct3D 12)  \n";
    std::cout << "====================================================\n\n";

    // 1. Inicializar Virtual File System
    Platform::VFS::Initialize("./game");

    // 2. Inicializar o espaço de memória virtual de 4 GB do console
    if (!Kernel::MemoryManager::Initialize()) {
        std::cerr << "[Main] Falha ao inicializar o gerenciador de memória do console." << std::endl;
        return 1;
    }

    // 3. Carregar seções do binário original descompactado no espaço 0x82000000
    auto pePath = Platform::VFS::ResolvePath("game:/default_unpacked.pe");
    if (std::filesystem::exists(pePath)) {
        Kernel::MemoryManager::LoadExecutableImage(pePath);
    } else {
        std::cerr << "[Main] Atenção: Imagem " << pePath << " não encontrada. Rode tools/unpack_xex.py primeiro.\n";
    }

    // 4. Mapear tabela de despacho de funções (PPCFuncMappings -> O(1) direct call table)
    Kernel::MemoryManager::InitializeFunctionDispatchTable();

    // 5. Inicializar chamadas HLE de Sistema Operacional (XboxKrnl & XAM)
    Kernel::InitializeExports();

    // 6. Teste de resolução de arquivo de nível
    auto mainMenuLvl = Platform::VFS::ResolvePath("game:/gl/common/gl_mainmenu.lvl_760");
    std::cout << "\n[Main] Verificando existência de menu principal: " 
              << mainMenuLvl.string() << " -> " 
              << (std::filesystem::exists(mainMenuLvl) ? "ENCONTRADO (OK)" : "NÃO ENCONTRADO") 
              << std::endl;

    // 7. Inicializar o Renderizador Nativo (Vulkan por padrão com fallback para Direct3D 12)
    Graphics::RenderConfig renderConfig;
    renderConfig.width = 1280;
    renderConfig.height = 720;
    renderConfig.backend = Graphics::Backend::Vulkan;

    if (!Graphics::NativeRenderer::Initialize(renderConfig)) {
        std::cerr << "[Main] Falha ao inicializar o renderizador nativo." << std::endl;
        return 1;
    }

    // 8. GPU emulada: o command processor consome o ring PM4 do jogo e chama o renderer a cada swap.
    //    Os imports Vd* (src/kernel/vd_exports.cpp) acessam este estado via GpuSystem::Get().
    static Graphics::RendererBackend rendererBackend;
    auto allocateGuest = [](uint32_t size) { return Kernel::MemoryManager::AllocateVirtual(size); };

    Gpu::GpuSystemConfig gpuConfig;
    gpuConfig.base = Kernel::MemoryManager::GetBase();
    gpuConfig.backend = &rendererBackend;
    gpuConfig.allocate = allocateGuest;
    gpuConfig.caller = Kernel::CreateGuestCaller(gpuConfig.base, allocateGuest);
    gpuConfig.enableInterrupts = gpuInterrupts;
    if (!Gpu::GpuSystem::Get().Initialize(gpuConfig)) {
        std::cerr << "[Main] Falha ao inicializar a GPU emulada." << std::endl;
        Graphics::NativeRenderer::Shutdown();
        return 1;
    }

    std::cout << "\n[Main] Sistema completamente inicializado e pronto para execução.\n";
    std::cout << "[Main] Feche a janela gráfica ou pressione ESC para encerrar o teste de runtime.\n";

    // 9. Iniciar a thread principal do jogo (entry point 0x822FC750 = _xstart)
    if (!Kernel::GuestThread::StartMain()) {
        std::cerr << "[Main] Falha ao iniciar a thread principal do guest." << std::endl;
        Gpu::GpuSystem::Get().Shutdown();
        Graphics::NativeRenderer::Shutdown();
        return 1;
    }

    // Loop da thread principal: so a janela. A apresentacao acontece na thread do CP (pacote XE_SWAP).
    while (Graphics::NativeRenderer::IsRunning() && Kernel::GuestThread::IsRunning()) {
        Graphics::NativeRenderer::PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (Kernel::GuestThread::IsRunning()) {
        // Janela fechada com o guest ainda executando: nao liberar a memoria sob a thread
        std::cout << "[Main] Encerrando com a thread do guest ativa." << std::endl;
        Gpu::GpuSystem::Get().Shutdown();
        Graphics::NativeRenderer::Shutdown();
        std::_Exit(0);
    }

    Gpu::GpuSystem::Get().Shutdown();
    Graphics::NativeRenderer::Shutdown();
    Kernel::MemoryManager::Shutdown();

    std::cout << "[Main] Execução finalizada com sucesso." << std::endl;
    return 0;
}
```

- [ ] **Step 6: Criar o executável de verificação manual**

`src/gpu_selftest/main.cpp`:
```cpp
// Executavel de verificacao manual do caminho de GPU, sem o jogo nem o codigo recompilado:
//   janela Plume (Vulkan/D3D12) + command processor + imports Vd*, alimentados por um "D3D" simulado
//   (Gpu::RunSelfTest). Uso: gpu_selftest [frames]
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <windows.h>

#include "graphics/renderer.h"
#include "gpu/gpu_registers.h"
#include "gpu/gpu_system.h"
#include "gpu/selftest.h"

namespace {
    constexpr GuestAddr kHeapBase = 0x40000000;
    constexpr uint32_t kHeapSize = 0x04000000;  // 64 MB

    // Espaco de 4 GB do guest: so o heap e a pagina MMIO da GPU sao commitados.
    uint8_t* CreateGuestMemory() {
        uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE));
        if (!base) return nullptr;
        if (!VirtualAlloc(base + kHeapBase, kHeapSize, MEM_COMMIT, PAGE_READWRITE) ||
            !VirtualAlloc(base + Gpu::Reg::kMmioBase, Gpu::Reg::kMmioSize, MEM_COMMIT, PAGE_READWRITE)) {
            return nullptr;
        }
        return base;
    }

    GuestAddr AllocateGuest(uint32_t size) {
        static std::atomic<GuestAddr> cursor{kHeapBase};
        return cursor.fetch_add((size + 0xFFFFu) & ~0xFFFFu);
    }
}

int main(int argc, char** argv) {
    const uint32_t frames = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 300;

    uint8_t* base = CreateGuestMemory();
    if (!base) {
        std::cerr << "[GpuSelfTest] Falha ao criar a memoria do guest." << std::endl;
        return 1;
    }

    Graphics::RenderConfig renderConfig;
    if (!Graphics::NativeRenderer::Initialize(renderConfig)) {
        std::cerr << "[GpuSelfTest] Falha ao inicializar o renderizador nativo." << std::endl;
        return 1;
    }

    Graphics::RendererBackend backend;
    Gpu::GpuSystemConfig config;
    config.base = base;
    config.backend = &backend;
    config.allocate = AllocateGuest;
    config.enableInterrupts = false;  // sem codigo guest para receber o callback
    if (!Gpu::GpuSystem::Get().Initialize(config)) {
        Graphics::NativeRenderer::Shutdown();
        return 1;
    }

    std::cout << "[GpuSelfTest] " << frames << " frames pelo ring PM4 (ESC ou fechar a janela encerra)." << std::endl;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
    std::thread feeder([&] {
        ok = Gpu::RunSelfTest(frames, 16, &cancel);
        done = true;
    });

    while (Graphics::NativeRenderer::IsRunning() && !done.load()) {
        Graphics::NativeRenderer::PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel = true;
    feeder.join();

    const Gpu::CpStats stats = Gpu::GpuSystem::Get().Cp().Stats();
    std::cout << "[GpuSelfTest] " << (ok ? "OK" : "FALHOU") << ": swaps=" << stats.swaps
              << " pacotes=" << stats.packets << " malformed=" << stats.malformed << std::endl;

    Gpu::GpuSystem::Get().Shutdown();
    Graphics::NativeRenderer::Shutdown();
    return (ok && stats.malformed == 0) ? 0 : 1;
}
```

- [ ] **Step 7: Editar o `CMakeLists.txt` raiz**

Na lista `SOURCES` do executável, acrescentar `src/kernel/guest_call.cpp` e `src/kernel/import_stubs.cpp`; trocar o `target_link_libraries` do executável para incluir `GpuCore`; e acrescentar o alvo `gpu_selftest` antes do bloco de testes. O trecho do executável e do selftest fica:

```cmake
# Definição do Executável Principal
# import_stubs.cpp é gerado por tools/gen_import_stubs.py (stubs dos imports sem implementação real).
set(SOURCES
    src/main.cpp
    src/platform/filesystem.cpp
    src/kernel/memory.cpp
    src/kernel/kernel_stubs.cpp
    src/kernel/guest_thread.cpp
    src/kernel/guest_call.cpp
    src/kernel/import_stubs.cpp
    src/graphics/renderer.cpp
)

add_executable(GreenLanternRecomp ${SOURCES})

target_link_libraries(GreenLanternRecomp PRIVATE RecompiledCode GpuCore plume)
if (WIN32)
    target_link_libraries(GreenLanternRecomp PRIVATE d3d12 dxgi dxguid)
endif()

# Configuração de Output
set_target_properties(GreenLanternRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Verificação manual do caminho de GPU sem o jogo e sem o código recompilado.
add_executable(gpu_selftest src/gpu_selftest/main.cpp src/graphics/renderer.cpp)
target_link_libraries(gpu_selftest PRIVATE GpuCore plume)
if (WIN32)
    target_link_libraries(gpu_selftest PRIVATE d3d12 dxgi dxguid)
endif()
set_target_properties(gpu_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
```

- [ ] **Step 8: Compilar `gpu_selftest` e rodar a suíte**

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_selftest gpu_tests -- /m /verbosity:minimal
.\build\Release\gpu_tests.exe
```
Expected: ambos compilam (a primeira vez compila `volk` e `plume`); `63 testes, 0 falharam`.

- [ ] **Step 9: Verificação manual em Vulkan**

```powershell
.\build\bin\Release\gpu_selftest.exe 120
```
Expected: a janela abre com o fundo verde-escuro por ~2 s e o console mostra, nesta ordem: `[Renderer] GPU: <nome da GPU>`, `[Renderer] Janela e contexto grafico nativo (Vulkan) criados com sucesso.`, `[GpuSelfTest] 120 frames pelo ring PM4 ...`, `[GpuSelfTest] OK: swaps=120 pacotes=6480 malformed=0`, e o processo termina com código 0 (`$LASTEXITCODE`). Se o log disser `Direct3D 12`, o Vulkan caiu no fallback: investigar o driver antes de seguir. Testar também fechar a janela ou apertar ESC no meio (`gpu_selftest.exe 100000`): deve encerrar sem travar.

- [ ] **Step 10: Compilar os fontes do executável principal (sem linkar)**

```powershell
cmake --build build --config Release --target GreenLanternRecomp -- /m /verbosity:minimal /p:BuildProjectReferences=false
```
Expected: `main.cpp`, `renderer.cpp`, `memory.cpp`, `guest_thread.cpp`, `guest_call.cpp` e `import_stubs.cpp` compilam sem erro. O build termina com `LINK : fatal error LNK1181: ... RecompiledCode.lib`: esperado (bloqueio conhecido). Qualquer **outro** erro é falha desta tarefa.

- [ ] **Step 11: Commit**

```powershell
git add CMakeLists.txt src/graphics/renderer.h src/graphics/renderer.cpp src/main.cpp src/kernel/memory.cpp src/kernel/guest_call.h src/kernel/guest_call.cpp src/gpu_selftest/main.cpp
git commit -m "Liga o command processor ao renderer Plume e adiciona o gpu_selftest" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Documentação e grafo do projeto

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: tudo acima
- Produces: nada (documentação)

- [ ] **Step 1: Atualizar o README**

Na árvore da seção "Estrutura do Projeto", trocar a linha de `graphics/` e acrescentar `gpu/`:
```text
│   ├── gpu/              # GPU emulada: command processor PM4, interrupcoes, imports Vd*
│   ├── graphics/         # Backend de renderização nativo (Plume / Vulkan / D3D12)
```
E acrescentar ao fim do README:
````markdown
## Testes e verificação da GPU

```powershell
cmake -S . -B build
cmake --build build --config Release --target gpu_tests gpu_selftest -- /m
ctest --test-dir build -C Release          # testes do núcleo da GPU
.\build\bin\Release\gpu_selftest.exe 120   # janela Vulkan alimentada por um D3D simulado
```

Os imports do kernel sem implementação real são stubs gerados: `python tools/gen_import_stubs.py`
(rode de novo depois de implementar um `__imp__*` em `src/kernel/*_exports.cpp`).
````

- [ ] **Step 2: Atualizar o grafo (instrução do CLAUDE.md do projeto)**

Run: `graphify update .`
Expected: conclui sem erro (AST apenas). Se `graphify` não estiver disponível, pular e avisar.

- [ ] **Step 3: Commit**

```powershell
git add README.md
git commit -m "Documenta testes e verificacao da GPU no README" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```
