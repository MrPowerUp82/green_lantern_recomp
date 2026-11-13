// Imports Rtl*, CRT (printf) e depuracao do xboxkrnl.exe.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace Kernel;

namespace {
    uint32_t Ld32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
    uint16_t Ld16(uint8_t* base, uint32_t addr) { return static_cast<uint16_t>((base[addr] << 8) | base[addr + 1]); }
    void St16(uint8_t* base, uint32_t addr, uint16_t value) { base[addr] = uint8_t(value >> 8); base[addr + 1] = uint8_t(value); }

    // ANSI_STRING / UNICODE_STRING: u16 length | u16 maximum_length | u32 buffer
    void WriteString(uint8_t* base, uint32_t dest, uint16_t length, uint16_t maxLength, uint32_t buffer) {
        St16(base, dest + 0, length);
        St16(base, dest + 2, maxLength);
        St32(base, dest + 4, buffer);
    }

    double FprValue(PPCContext& ctx, uint32_t index) {
        switch (index) {
        case 1: return ctx.f1.f64;   case 2: return ctx.f2.f64;   case 3: return ctx.f3.f64;
        case 4: return ctx.f4.f64;   case 5: return ctx.f5.f64;   case 6: return ctx.f6.f64;
        case 7: return ctx.f7.f64;   case 8: return ctx.f8.f64;   case 9: return ctx.f9.f64;
        case 10: return ctx.f10.f64; case 11: return ctx.f11.f64; case 12: return ctx.f12.f64;
        case 13: return ctx.f13.f64;
        default: return 0.0;
        }
    }

    uint64_t GprValue(PPCContext& ctx, uint32_t index) {
        switch (index) {
        case 3: return ctx.r3.u64; case 4: return ctx.r4.u64; case 5: return ctx.r5.u64;
        case 6: return ctx.r6.u64; case 7: return ctx.r7.u64; case 8: return ctx.r8.u64;
        case 9: return ctx.r9.u64; case 10: return ctx.r10.u64;
        default: return 0;
        }
    }

    // Leitor de argumentos variaveis: ou registradores/pilha (sprintf, _snprintf) ou um va_list no
    // guest (_vsnprintf), onde cada argumento ocupa um slot de 8 bytes (valores de 32 bits nos 4 bytes finais).
    class ArgReader {
    public:
        // Argumentos variaveis a partir do registrador `firstGpr`.
        ArgReader(PPCContext& ctx, uint8_t* base, uint32_t firstGpr) : m_ctx(&ctx), m_base(base), m_gpr(firstGpr) {}
        // Argumentos num va_list do guest.
        ArgReader(uint8_t* base, uint32_t vaList) : m_base(base), m_vaList(vaList) {}

        uint64_t Next64() {
            if (m_ctx) {
                const uint32_t slot = m_gpr++;
                if (slot <= 10) return GprValue(*m_ctx, slot);
                const uint32_t addr = m_ctx->r1.u32 + 0x54 + (slot - 11) * 8;
                return Endian::Swap64(*reinterpret_cast<uint64_t*>(m_base + addr));
            }
            const uint32_t addr = m_vaList + 8 * m_vaIndex++;
            return Endian::Swap64(*reinterpret_cast<uint64_t*>(m_base + addr));
        }

        uint32_t Next32() { return static_cast<uint32_t>(Next64()); }

        double NextDouble() {
            if (m_ctx) {
                ++m_gpr;  // o double tambem ocupa um slot de GPR
                return FprValue(*m_ctx, 1 + m_fpr++);
            }
            const uint64_t bits = Next64();
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        const char* NextString() {
            const uint32_t ptr = Next32();
            return ptr ? reinterpret_cast<const char*>(m_base + ptr) : "(null)";
        }

    private:
        PPCContext* m_ctx = nullptr;
        uint8_t* m_base;
        uint32_t m_gpr = 0;
        uint32_t m_fpr = 0;
        uint32_t m_vaList = 0;
        uint32_t m_vaIndex = 0;
    };

    // Implementacao de printf sobre argumentos do guest. Cada especificador e formatado pelo
    // snprintf do host com os mesmos flags/largura/precisao.
    std::string FormatGuest(const char* format, ArgReader& args) {
        std::string out;
        for (const char* p = format; *p;) {
            if (*p != '%') { out.push_back(*p++); continue; }
            const char* start = p++;
            if (*p == '%') { out.push_back('%'); ++p; continue; }

            std::string spec = "%";
            while (std::strchr("-+ #0", *p) && *p) spec.push_back(*p++);
            if (*p == '*') { spec += std::to_string(static_cast<int32_t>(args.Next32())); ++p; }
            while (std::isdigit(static_cast<unsigned char>(*p))) spec.push_back(*p++);
            if (*p == '.') {
                spec.push_back(*p++);
                if (*p == '*') { spec += std::to_string(static_cast<int32_t>(args.Next32())); ++p; }
                while (std::isdigit(static_cast<unsigned char>(*p))) spec.push_back(*p++);
            }

            bool longLong = false;
            bool wide = false;
            while (*p == 'l' || *p == 'h' || *p == 'I' || *p == 'z' || *p == 't' || *p == 'j' || *p == 'w') {
                if (*p == 'l' && p[1] == 'l') { longLong = true; ++p; }
                else if (*p == 'I' && p[1] == '6' && p[2] == '4') { longLong = true; p += 2; }
                else if (*p == 'w' || *p == 'l') wide = (*p == 'w') || wide;
                ++p;
            }
            (void)wide;

            char buffer[512];
            const char conv = *p;
            if (!conv) { out.append(start); break; }
            ++p;
            switch (conv) {
            case 'd': case 'i': {
                spec += longLong ? "lld" : "d";
                const int n = longLong ? std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<long long>(args.Next64()))
                                       : std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<int32_t>(args.Next32()));
                out.append(buffer, n > 0 ? std::min<int>(n, sizeof(buffer) - 1) : 0);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                spec += longLong ? "ll" : "";
                spec.push_back(conv);
                const int n = longLong ? std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<unsigned long long>(args.Next64()))
                                       : std::snprintf(buffer, sizeof(buffer), spec.c_str(), args.Next32());
                out.append(buffer, n > 0 ? std::min<int>(n, sizeof(buffer) - 1) : 0);
                break;
            }
            case 'c': {
                spec.push_back('c');
                const int n = std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<int>(args.Next32() & 0xFF));
                out.append(buffer, n > 0 ? std::min<int>(n, sizeof(buffer) - 1) : 0);
                break;
            }
            case 's': {
                spec.push_back('s');
                const char* text = args.NextString();
                const int n = std::snprintf(buffer, sizeof(buffer), spec.c_str(), text);
                out.append(buffer, n > 0 ? std::min<int>(n, sizeof(buffer) - 1) : 0);
                break;
            }
            case 'p': {
                std::snprintf(buffer, sizeof(buffer), "%08X", args.Next32());
                out.append(buffer);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                spec.push_back(conv);
                const int n = std::snprintf(buffer, sizeof(buffer), spec.c_str(), args.NextDouble());
                out.append(buffer, n > 0 ? std::min<int>(n, sizeof(buffer) - 1) : 0);
                break;
            }
            default:
                out.append(start, p - start);
                break;
            }
        }
        return out;
    }

    // Copia o texto formatado para o buffer do guest (semantica de _snprintf: retorna o tamanho
    // total, ou -1 se truncou).
    int32_t StoreFormatted(uint8_t* base, uint32_t dest, uint32_t capacity, const std::string& text) {
        if (!dest || capacity == 0) return static_cast<int32_t>(text.size());
        const size_t copy = std::min<size_t>(text.size(), capacity);
        std::memcpy(base + dest, text.data(), copy);
        if (copy < capacity) base[dest + copy] = 0;
        return text.size() > capacity ? -1 : static_cast<int32_t>(text.size());
    }
}

// --- Strings ---------------------------------------------------------------------------------

// r3 = ANSI_STRING*, r4 = char*
KERNEL_FUNC(__imp__RtlInitAnsiString) {
    const uint32_t src = ctx.r4.u32;
    const size_t length = src ? std::strlen(reinterpret_cast<const char*>(base + src)) : 0;
    WriteString(base, ctx.r3.u32, static_cast<uint16_t>(length), static_cast<uint16_t>(src ? length + 1 : 0), src);
}

// r3 = UNICODE_STRING*, r4 = wchar_t* (big-endian)
KERNEL_FUNC(__imp__RtlInitUnicodeString) {
    const uint32_t src = ctx.r4.u32;
    uint32_t length = 0;
    if (src) while (Ld16(base, src + length * 2) != 0) ++length;
    WriteString(base, ctx.r3.u32, static_cast<uint16_t>(length * 2), static_cast<uint16_t>(src ? length * 2 + 2 : 0), src);
}

KERNEL_FUNC(__imp__RtlFreeAnsiString) { (void)ctx; }  // o buffer vem do pool (sem liberacao)

// r3 = ANSI_STRING* destino, r4 = UNICODE_STRING* origem, r5 = alocar buffer
KERNEL_FUNC(__imp__RtlUnicodeStringToAnsiString) {
    const uint32_t dest = ctx.r3.u32;
    const uint32_t src = ctx.r4.u32;
    const uint32_t chars = Ld16(base, src) / 2;
    const uint32_t srcBuffer = Ld32(base, src + 4);
    uint32_t buffer = Ld32(base, dest + 4);
    if (ctx.r5.u32) {
        buffer = PoolAlloc(chars + 1);
        if (!buffer) { ctx.r3.u64 = Status::kNoMemory; return; }
        WriteString(base, dest, 0, static_cast<uint16_t>(chars + 1), buffer);
    } else if (Ld16(base, dest + 2) < chars) {
        ctx.r3.u64 = Status::kBufferOverflow;
        return;
    }
    for (uint32_t i = 0; i < chars; i++) base[buffer + i] = static_cast<uint8_t>(Ld16(base, srcBuffer + i * 2));
    base[buffer + chars] = 0;
    St16(base, dest, static_cast<uint16_t>(chars));
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = wchar_t* destino, r4 = capacidade em bytes, r5 = bytes escritos*, r6 = char* origem, r7 = tamanho
KERNEL_FUNC(__imp__RtlMultiByteToUnicodeN) {
    const uint32_t maxChars = ctx.r4.u32 / 2;
    const uint32_t count = std::min(maxChars, ctx.r7.u32);
    for (uint32_t i = 0; i < count; i++) St16(base, ctx.r3.u32 + i * 2, base[ctx.r6.u32 + i]);
    if (ctx.r5.u32) St32(base, ctx.r5.u32, count * 2);
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__RtlUpcaseUnicodeChar) {
    const uint32_t c = ctx.r3.u32 & 0xFFFF;
    ctx.r3.u64 = (c >= 'a' && c <= 'z') ? c - 32 : c;
}

// r3 = string1, r4 = len1, r5 = string2, r6 = len2, r7 = ignorar maiusculas -> -1 / 0 / 1
KERNEL_FUNC(__imp__RtlCompareStringN) {
    const uint32_t len1 = ctx.r4.u32;
    const uint32_t len2 = ctx.r6.u32;
    const bool fold = ctx.r7.u32 != 0;
    const uint32_t common = std::min(len1, len2);
    int32_t result = 0;
    for (uint32_t i = 0; i < common && result == 0; i++) {
        int a = base[ctx.r3.u32 + i];
        int b = base[ctx.r5.u32 + i];
        if (fold) { a = std::toupper(a); b = std::toupper(b); }
        result = a < b ? -1 : (a > b ? 1 : 0);
    }
    if (result == 0) result = len1 < len2 ? -1 : (len1 > len2 ? 1 : 0);
    ctx.r3.u64 = static_cast<uint64_t>(static_cast<int64_t>(result));
}

// r3 = endereco, r4 = tamanho em bytes, r5 = padrao de 32 bits -> bytes iguais ao padrao
KERNEL_FUNC(__imp__RtlCompareMemoryUlong) {
    const uint32_t pattern = ctx.r5.u32;
    uint32_t matched = 0;
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4) {
        if (Ld32(base, ctx.r3.u32 + i) != pattern) break;
        matched += 4;
    }
    ctx.r3.u64 = matched;
}

KERNEL_FUNC(__imp__RtlFillMemoryUlong) {
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4) St32(base, ctx.r3.u32 + i, ctx.r5.u32);
}

KERNEL_FUNC(__imp__RtlNtStatusToDosError) {
    switch (ctx.r3.u32) {
    case Status::kSuccess: ctx.r3.u64 = 0; break;
    case Status::kInvalidHandle: ctx.r3.u64 = 6; break;       // ERROR_INVALID_HANDLE
    case Status::kObjectNameNotFound:
    case Status::kNoSuchFile: ctx.r3.u64 = 2; break;          // ERROR_FILE_NOT_FOUND
    case Status::kObjectPathNotFound: ctx.r3.u64 = 3; break;  // ERROR_PATH_NOT_FOUND
    case Status::kAccessDenied: ctx.r3.u64 = 5; break;        // ERROR_ACCESS_DENIED
    case Status::kNoMemory: ctx.r3.u64 = 8; break;            // ERROR_NOT_ENOUGH_MEMORY
    case Status::kInvalidParameter: ctx.r3.u64 = 87; break;   // ERROR_INVALID_PARAMETER
    case Status::kEndOfFile: ctx.r3.u64 = 38; break;          // ERROR_HANDLE_EOF
    case Status::kNoMoreFiles: ctx.r3.u64 = 18; break;        // ERROR_NO_MORE_FILES
    case Status::kTimeout: ctx.r3.u64 = 258; break;           // WAIT_TIMEOUT
    default: ctx.r3.u64 = 317; break;                         // ERROR_MR_MID_NOT_FOUND
    }
    (void)base;
}

// --- printf ----------------------------------------------------------------------------------

// sprintf(r3 = buffer, r4 = formato, ...)
KERNEL_FUNC(__imp__sprintf) {
    ArgReader args(ctx, base, 5);
    const std::string text = FormatGuest(reinterpret_cast<const char*>(base + ctx.r4.u32), args);
    StoreFormatted(base, ctx.r3.u32, 0x7FFFFFFF, text);
    ctx.r3.u64 = text.size();
}

// DbgPrint(r3 = formato, ...)
KERNEL_FUNC(__imp__DbgPrint) {
    ArgReader args(ctx, base, 4);
    const std::string text = FormatGuest(reinterpret_cast<const char*>(base + ctx.r3.u32), args);
    std::fprintf(stderr, "[DbgPrint] %s", text.c_str());
    if (text.empty() || text.back() != '\n') std::fputc('\n', stderr);
    ctx.r3.u64 = 0;
}

// _snprintf(r3 = buffer, r4 = capacidade, r5 = formato, ...)
KERNEL_FUNC(__imp___snprintf) {
    ArgReader args(ctx, base, 6);
    const std::string text = FormatGuest(reinterpret_cast<const char*>(base + ctx.r5.u32), args);
    ctx.r3.u64 = static_cast<uint64_t>(static_cast<int64_t>(StoreFormatted(base, ctx.r3.u32, ctx.r4.u32, text)));
}

// _vsnprintf(r3 = buffer, r4 = capacidade, r5 = formato, r6 = va_list*)
KERNEL_FUNC(__imp___vsnprintf) {
    ArgReader args(base, ctx.r6.u32);
    const std::string text = FormatGuest(reinterpret_cast<const char*>(base + ctx.r5.u32), args);
    ctx.r3.u64 = static_cast<uint64_t>(static_cast<int64_t>(StoreFormatted(base, ctx.r3.u32, ctx.r4.u32, text)));
}

// --- Depuracao e falhas ------------------------------------------------------------------------

KERNEL_FUNC(__imp__DbgBreakPoint) { (void)ctx; (void)base; }

KERNEL_FUNC(__imp__KeBugCheck) {
    std::fprintf(stderr, "[Kernel] KeBugCheck(0x%08X)\n", ctx.r3.u32);
    (void)base;
    std::fflush(stderr);
    std::_Exit(3);
}

KERNEL_FUNC(__imp__KeBugCheckEx) {
    std::fprintf(stderr, "[Kernel] KeBugCheckEx(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X)\n",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    (void)base;
    std::fflush(stderr);
    std::_Exit(3);
}

KERNEL_FUNC(__imp__RtlRaiseException) {
    // r3 = EXCEPTION_RECORD*: codigo da excecao no primeiro dword
    std::fprintf(stderr, "[Kernel] RtlRaiseException codigo 0x%08X\n", ctx.r3.u32 ? Ld32(base, ctx.r3.u32) : 0);
}

KERNEL_FUNC(__imp__RtlCaptureContext) { (void)ctx; (void)base; }
KERNEL_FUNC(__imp__RtlUnwind) { (void)ctx; (void)base; }
KERNEL_FUNC(__imp____C_specific_handler) { ctx.r3.u64 = 1; (void)base; }
