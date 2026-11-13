// Imports do xam.xex: entrada (controle e teclado), usuario, notificacoes e tarefas.
//
// Sem controle XInput conectado, o teclado simula o controle 0 (mapeamento em README.md).

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <xinput.h>
#endif

using namespace Kernel;

namespace {
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorDeviceNotConnected = 0x048F;
    constexpr uint32_t kErrorNoSuchUser = 0x0525;
    constexpr uint32_t kErrorEmpty = 0x10D2;

    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
    void St16(uint8_t* base, uint32_t addr, uint16_t value) { base[addr] = uint8_t(value >> 8); base[addr + 1] = uint8_t(value); }

    // XINPUT_GAMEPAD do Xbox 360 (mesmo layout do XInput do PC).
    struct GamepadState {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0;
        uint8_t rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
        bool operator==(const GamepadState& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    };

    constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
    constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
    constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200;
    constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000, kButtonX = 0x4000, kButtonY = 0x8000;

#if defined(_WIN32)
    bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool OwnWindowFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    GamepadState KeyboardState() {
        GamepadState state;
        // GL_AUTO_INPUT=1 (teste sem teclado): pulsa Start e A de 3 em 3 segundos, 300 ms cada.
        // GL_AUTO_INPUT=2: tambem pulsa direcional para cima antes do A (escolhe a primeira opcao dos menus).
        static const int autoInput = std::getenv("GL_AUTO_INPUT") ? std::atoi(std::getenv("GL_AUTO_INPUT")) : 0;
        if (autoInput) {
            const uint64_t ms = GetTickCount64() % 3000;
            if (ms < 300) state.buttons |= kStart;
            else if (autoInput >= 2 && ms >= 800 && ms < 1000) state.buttons |= kDpadUp;
            else if (ms >= 1500 && ms < 1800) state.buttons |= kButtonA;
            return state;
        }
        if (!OwnWindowFocused()) return state;
        auto axis = [](bool negative, bool positive) -> int16_t { return negative ? -32767 : (positive ? 32767 : 0); };
        state.thumbLX = axis(KeyDown('A'), KeyDown('D'));
        state.thumbLY = axis(KeyDown('S'), KeyDown('W'));
        state.thumbRX = axis(KeyDown(VK_LEFT), KeyDown(VK_RIGHT));
        state.thumbRY = axis(KeyDown(VK_DOWN), KeyDown(VK_UP));
        if (KeyDown(VK_SPACE)) state.buttons |= kButtonA;
        if (KeyDown('E')) state.buttons |= kButtonB;
        if (KeyDown('Q')) state.buttons |= kButtonX;
        if (KeyDown('R')) state.buttons |= kButtonY;
        if (KeyDown('1')) state.buttons |= kLeftShoulder;
        if (KeyDown('3')) state.buttons |= kRightShoulder;
        if (KeyDown(VK_RETURN)) state.buttons |= kStart;
        if (KeyDown(VK_BACK)) state.buttons |= kBack;
        if (KeyDown(VK_LCONTROL)) state.buttons |= kLeftThumb;
        if (KeyDown('C')) state.buttons |= kRightThumb;
        if (KeyDown('I')) state.buttons |= kDpadUp;
        if (KeyDown('K')) state.buttons |= kDpadDown;
        if (KeyDown('J')) state.buttons |= kDpadLeft;
        if (KeyDown('L')) state.buttons |= kDpadRight;
        if (KeyDown(VK_LSHIFT)) state.leftTrigger = 255;
        if (KeyDown('Z')) state.rightTrigger = 255;
        return state;
    }

    // Controle real (XInput) se estiver conectado; senao o teclado, so para o usuario 0.
    bool ReadGamepad(uint32_t user, GamepadState& out) {
        XINPUT_STATE xs;
        if (user < 4 && XInputGetState(user, &xs) == ERROR_SUCCESS) {
            out.buttons = xs.Gamepad.wButtons;
            out.leftTrigger = xs.Gamepad.bLeftTrigger;
            out.rightTrigger = xs.Gamepad.bRightTrigger;
            out.thumbLX = xs.Gamepad.sThumbLX;
            out.thumbLY = xs.Gamepad.sThumbLY;
            out.thumbRX = xs.Gamepad.sThumbRX;
            out.thumbRY = xs.Gamepad.sThumbRY;
            return true;
        }
        if (user == 0) {
            out = KeyboardState();
            return true;
        }
        return false;
    }
#else
    bool ReadGamepad(uint32_t, GamepadState&) { return false; }
#endif

    void WriteGamepad(uint8_t* base, uint32_t ptr, const GamepadState& pad) {
        St16(base, ptr + 0, pad.buttons);
        base[ptr + 2] = pad.leftTrigger;
        base[ptr + 3] = pad.rightTrigger;
        St16(base, ptr + 4, static_cast<uint16_t>(pad.thumbLX));
        St16(base, ptr + 6, static_cast<uint16_t>(pad.thumbLY));
        St16(base, ptr + 8, static_cast<uint16_t>(pad.thumbRX));
        St16(base, ptr + 10, static_cast<uint16_t>(pad.thumbRY));
    }

    std::mutex g_inputLock;
    GamepadState g_lastState[4];
    uint32_t g_packet[4] = {};

    // Handle falso para listeners de notificacao (nunca recebem eventos).
    class KNotifyListener : public KObject {
    public:
        KNotifyListener() : KObject(Type::Other) {}
        bool Signaled(uint32_t) const override { return false; }
    };
}

// --- Entrada ----------------------------------------------------------------------------------

// r3 = usuario, r4 = flags, r5 = X_INPUT_STATE* (packet u32 + X_INPUT_GAMEPAD)
KERNEL_FUNC(__imp__XamInputGetState) {
    const uint32_t user = ctx.r3.u32 & 0xFF;
    GamepadState pad;
    if (user >= 4 || !ReadGamepad(user, pad)) { ctx.r3.u64 = kErrorDeviceNotConnected; return; }

    uint32_t packet;
    {
        std::lock_guard<std::mutex> lock(g_inputLock);
        if (!(g_lastState[user] == pad)) { g_lastState[user] = pad; ++g_packet[user]; }
        packet = g_packet[user];
    }
    if (ctx.r5.u32) {
        St32(base, ctx.r5.u32, packet);
        WriteGamepad(base, ctx.r5.u32 + 4, pad);
    }
    ctx.r3.u64 = kErrorSuccess;
}

// r3 = usuario, r4 = flags, r5 = X_INPUT_CAPABILITIES*
KERNEL_FUNC(__imp__XamInputGetCapabilities) {
    const uint32_t user = ctx.r3.u32 & 0xFF;
    GamepadState pad;
    if (user >= 4 || !ReadGamepad(user, pad)) { ctx.r3.u64 = kErrorDeviceNotConnected; return; }
    if (ctx.r5.u32) {
        std::memset(base + ctx.r5.u32, 0, 20);
        base[ctx.r5.u32 + 0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
        base[ctx.r5.u32 + 1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
        GamepadState full;
        full.buttons = 0xF3FF;
        full.leftTrigger = full.rightTrigger = 255;
        full.thumbLX = full.thumbLY = full.thumbRX = full.thumbRY = static_cast<int16_t>(0xFFC0);
        WriteGamepad(base, ctx.r5.u32 + 4, full);
        St16(base, ctx.r5.u32 + 16, 0xFFFF);  // vibracao: motores esquerdo/direito
        St16(base, ctx.r5.u32 + 18, 0xFFFF);
    }
    ctx.r3.u64 = kErrorSuccess;
}

KERNEL_FUNC(__imp__XamInputSetState) { ctx.r3.u64 = kErrorSuccess; (void)base; }
KERNEL_FUNC(__imp__XamInputGetKeystrokeEx) { ctx.r3.u64 = kErrorEmpty; (void)base; }

// --- Usuario e sistema -------------------------------------------------------------------------

KERNEL_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = (ctx.r3.u32 == 0) ? 1 : 0; (void)base; }  // 1 = local

// r3 = usuario, r4 = tipo, r5 = XUID*
KERNEL_FUNC(__imp__XamUserGetXUID) {
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = kErrorNoSuchUser; return; }
    if (ctx.r5.u32) {
        const uint64_t xuid = 0xE000000000000001ull;
        *reinterpret_cast<uint64_t*>(base + ctx.r5.u32) = Endian::Swap64(xuid);
    }
    ctx.r3.u64 = kErrorSuccess;
}

KERNEL_FUNC(__imp__XamGetSystemVersion) { ctx.r3.u64 = 0x20000000; (void)base; }
KERNEL_FUNC(__imp__XGetLanguage) { ctx.r3.u64 = 1; (void)base; }     // ingles
KERNEL_FUNC(__imp__XGetAVPack) { ctx.r3.u64 = 6; (void)base; }       // HDMI
KERNEL_FUNC(__imp__XGetGameRegion) { ctx.r3.u64 = 0x00FF; (void)base; }

// r3 = X_VIDEO_MODE*
KERNEL_FUNC(__imp__XGetVideoMode) {
    if (!ctx.r3.u32) return;
    std::memset(base + ctx.r3.u32, 0, 48);
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, sizeof(refreshBits));
    St32(base, ctx.r3.u32 + 0x00, 1280);
    St32(base, ctx.r3.u32 + 0x04, 720);
    St32(base, ctx.r3.u32 + 0x0C, 1);  // widescreen
    St32(base, ctx.r3.u32 + 0x10, 1);  // alta definicao
    St32(base, ctx.r3.u32 + 0x14, refreshBits);
    St32(base, ctx.r3.u32 + 0x18, 1);
}

KERNEL_FUNC(__imp__XamLoaderTerminateTitle) {
    (void)ctx; (void)base;
    std::cerr << "[Kernel] o jogo pediu para encerrar (XamLoaderTerminateTitle)" << std::endl;
    std::_Exit(0);
}

// r3 = categoria, r4 = configuracao, r5 = buffer, r6 = tamanho, r7 = tamanho necessario*
KERNEL_FUNC(__imp__ExGetXConfigSetting) {
    if (ctx.r5.u32 && ctx.r6.u32) std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
    if (ctx.r7.u32) St32(base, ctx.r7.u32, ctx.r6.u32);
    ctx.r3.u64 = 0;
}

// --- Notificacoes e tarefas ---------------------------------------------------------------------

// r3 = mascara -> handle do listener
KERNEL_FUNC(__imp__XamNotifyCreateListener) {
    (void)base;
    ctx.r3.u64 = AddHandle(std::make_shared<KNotifyListener>());
}

// r3 = listener, r4 = id desejado, r5 = parametro* -> 0 (nenhuma notificacao)
KERNEL_FUNC(__imp__XNotifyGetNext) { ctx.r3.u64 = 0; (void)base; }
KERNEL_FUNC(__imp__XNotifyPositionUI) { (void)ctx; (void)base; }

// r3 = callback, r4 = mensagem, r5 = opcional, r6 = handle* : roda a tarefa numa thread propria
KERNEL_FUNC(__imp__XamTaskSchedule) {
    ThreadStart start;
    start.entry = ctx.r3.u32;
    start.argument = ctx.r4.u32;
    start.stackSize = 0x40000;
    auto thread = StartGuestThread(base, start);
    if (!thread) { ctx.r3.u64 = Status::kNoMemory; return; }
    const uint32_t handle = AddHandle(thread);
    if (ctx.r6.u32) St32(base, ctx.r6.u32, handle);
    ctx.r3.u64 = kErrorSuccess;
}

KERNEL_FUNC(__imp__XamTaskShouldExit) { ctx.r3.u64 = 0; (void)base; }
KERNEL_FUNC(__imp__XamTaskCloseHandle) { RemoveHandle(ctx.r3.u32); ctx.r3.u64 = kErrorSuccess; (void)base; }

// --- Conteudo (saves) e dialogos --------------------------------------------------------------
// O port nao tem perfil Live: ha um "HDD" (id 1) e os dialogos do dashboard concluem na hora. Operacoes
// assincronas completam o XOVERLAPPED imediatamente.
//
// Pacotes de conteudo (saves) viram pastas em save/content/<tipo>/<nome do arquivo>/, montadas no nome
// raiz pedido pelo jogo (ex.: "save:"). Ao lado de cada pasta, <nome>.xcd guarda o XCONTENT_DATA original
// (nome de exibicao etc.) para a enumeracao.

namespace {
    constexpr uint32_t kErrorFileNotFound = 2;
    constexpr uint32_t kErrorIoPending = 997;
    constexpr uint32_t kErrorNoMoreFiles = 18;
    constexpr uint32_t kErrorAlreadyExists = 183;
    constexpr uint32_t kErrorInvalidParameter = 87;
    constexpr uint32_t kXContentDataSize = 0x134;
    constexpr uint32_t kHddDeviceId = 1;

    // XCONTENT_DATA: +0 dispositivo, +4 tipo, +8 nome de exibicao (UTF-16 BE, 128), +0x108 arquivo (char 42).
    struct ContentData {
        uint8_t raw[kXContentDataSize] = {};
        uint32_t ContentType() const { return Endian::ReadBe32(raw + 4); }
        std::string FileName() const {
            const char* name = reinterpret_cast<const char*>(raw + 0x108);
            return std::string(name, strnlen(name, 42));
        }
    };

    ContentData ReadContentData(uint8_t* base, uint32_t addr) {
        ContentData data;
        std::memcpy(data.raw, base + addr, kXContentDataSize);
        return data;
    }

    // Nome do arquivo vira pasta do host: so caracteres seguros.
    bool SafeFileName(const std::string& name) {
        if (name.empty() || name == "." || name == "..") return false;
        for (unsigned char c : name) {
            if (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) return false;
        }
        return true;
    }

    std::filesystem::path ContentTypeDir(uint32_t contentType) {
        char type[16];
        std::snprintf(type, sizeof(type), "%08X", contentType);
        return ContentRoot() / type;
    }

    std::filesystem::path ContentPath(const ContentData& data) { return ContentTypeDir(data.ContentType()) / data.FileName(); }

    class KEnumerator : public KObject {
    public:
        KEnumerator() : KObject(Type::Other) {}
        bool Signaled(uint32_t) const override { return true; }

        std::vector<ContentData> items;
        size_t next = 0;
        uint32_t itemsPerCall = 1;
    };

    // XOVERLAPPED: +0 resultado (0x103 = pendente), +4 tamanho/itens, +8 erro estendido, +0xC evento.
    // Devolve o codigo que a API assincrona deve retornar (ERROR_IO_PENDING) ou o proprio resultado.
    uint32_t CompleteOverlapped(uint8_t* base, uint32_t overlapped, uint32_t result, uint32_t length) {
        if (!overlapped) return result;
        St32(base, overlapped + 0x00, result);
        St32(base, overlapped + 0x04, length);
        St32(base, overlapped + 0x08, 0);
        const uint32_t eventHandle = Endian::ReadBe32(base + overlapped + 0x0C);
        if (eventHandle) {
            auto object = LookupHandle(eventHandle);
            if (object && object->type == KObject::Type::Event) SetEvent(static_cast<KEvent&>(*object), base);
        }
        return kErrorIoPending;
    }
}

// r3 = usuario, r4 = dispositivo, r5 = tipo de conteudo, r6 = flags, r7 = itens por chamada,
// r8 = tamanho do buffer*, r9 = handle*
KERNEL_FUNC(__imp__XamContentCreateEnumerator) {
    const uint32_t device = ctx.r4.u32, contentType = ctx.r5.u32;
    auto enumerator = std::make_shared<KEnumerator>();
    enumerator->itemsPerCall = std::max(ctx.r7.u32, 1u);
    if (device == 0 || device == kHddDeviceId) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(ContentTypeDir(contentType), ec)) {
            if (entry.path().extension() != ".xcd") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            ContentData data;
            if (!file.read(reinterpret_cast<char*>(data.raw), kXContentDataSize)) continue;
            Endian::WriteBe32(data.raw, kHddDeviceId);
            if (!std::filesystem::is_directory(ContentPath(data), ec)) continue;
            enumerator->items.push_back(data);
        }
    }
    if (ctx.r8.u32) St32(base, ctx.r8.u32, kXContentDataSize * enumerator->itemsPerCall);
    if (ctx.r9.u32) St32(base, ctx.r9.u32, AddHandle(enumerator));
    ctx.r3.u64 = kErrorSuccess;
}

// r3 = handle, r4 = flags, r5 = buffer, r6 = tamanho do buffer, r7 = itens retornados*, r8 = XOVERLAPPED*
KERNEL_FUNC(__imp__XamEnumerate) {
    auto object = LookupHandle(ctx.r3.u32);
    auto* enumerator = dynamic_cast<KEnumerator*>(object.get());
    uint32_t count = 0;
    if (enumerator && ctx.r5.u32) {
        const uint32_t capacity = std::min(enumerator->itemsPerCall, ctx.r6.u32 / kXContentDataSize);
        std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
        while (count < capacity && enumerator->next < enumerator->items.size()) {
            std::memcpy(base + ctx.r5.u32 + count * kXContentDataSize, enumerator->items[enumerator->next++].raw, kXContentDataSize);
            ++count;
        }
    }
    const uint32_t result = count ? kErrorSuccess : kErrorNoMoreFiles;
    if (ctx.r7.u32) St32(base, ctx.r7.u32, count);
    ctx.r3.u64 = ctx.r8.u32 ? CompleteOverlapped(base, ctx.r8.u32, result, count) : result;
}

// r3 = usuario, r4 = nome raiz (char*), r5 = XCONTENT_DATA*, r6 = flags (disposicao em 0xF), r7 = disposicao*,
// r8 = mascara de licenca*, r9 = tamanho do cache, r10 = tamanho do conteudo, pilha[0] = XOVERLAPPED*
KERNEL_FUNC(__imp__XamContentCreateEx) {
    const uint32_t overlapped = Endian::ReadBe32(base + ctx.r1.u32 + 0x54);
    const std::string root = ctx.r4.u32 ? std::string(reinterpret_cast<const char*>(base + ctx.r4.u32)) : std::string();
    uint32_t result = kErrorSuccess, disposition = 0;
    if (root.empty() || !ctx.r5.u32) {
        result = kErrorInvalidParameter;
    } else {
        const ContentData data = ReadContentData(base, ctx.r5.u32);
        if (!SafeFileName(data.FileName())) {
            result = kErrorInvalidParameter;
        } else {
            const std::filesystem::path path = ContentPath(data);
            std::error_code ec;
            const bool exists = std::filesystem::is_directory(path, ec);
            constexpr uint32_t kCreated = 1, kOpened = 2;
            switch (ctx.r6.u32 & 0xF) {
            case 1:  // CREATE_NEW
                if (exists) result = kErrorAlreadyExists; else disposition = kCreated;
                break;
            case 2:  // CREATE_ALWAYS
            case 4:  // OPEN_ALWAYS
                disposition = exists ? kOpened : kCreated;
                break;
            case 3:  // OPEN_EXISTING
                if (!exists) result = kErrorFileNotFound; else disposition = kOpened;
                break;
            case 5:  // TRUNCATE_EXISTING
                if (!exists) {
                    result = kErrorFileNotFound;
                } else {
                    std::filesystem::remove_all(path, ec);
                    disposition = kOpened;
                }
                break;
            default:
                result = kErrorInvalidParameter;
                break;
            }
            if (result == kErrorSuccess) {
                std::filesystem::create_directories(path, ec);
                if (disposition == kCreated || !std::filesystem::exists(path.string() + ".xcd")) {
                    std::ofstream(path.string() + ".xcd", std::ios::binary)
                        .write(reinterpret_cast<const char*>(data.raw), kXContentDataSize);
                }
                MountDevice(root, path);
            }
            std::fprintf(stderr, "[XAM] XamContentCreateEx %s -> %s (flags %X, resultado %u, disposicao %u)\n", root.c_str(),
                         path.string().c_str(), ctx.r6.u32, result, disposition);
        }
    }
    if (ctx.r7.u32) St32(base, ctx.r7.u32, disposition);
    if (ctx.r8.u32 && result == kErrorSuccess) St32(base, ctx.r8.u32, 0);
    ctx.r3.u64 = overlapped ? CompleteOverlapped(base, overlapped, result, 0) : result;
}

// r3 = nome raiz (char*), r4 = XOVERLAPPED*
KERNEL_FUNC(__imp__XamContentClose) {
    if (ctx.r3.u32) UnmountDevice(reinterpret_cast<const char*>(base + ctx.r3.u32));
    ctx.r3.u64 = ctx.r4.u32 ? CompleteOverlapped(base, ctx.r4.u32, kErrorSuccess, 0) : kErrorSuccess;
}

// r3 = usuario, r4 = XCONTENT_DATA*, r5 = e criador* (u32), r6 = xuid do criador* (u64), r7 = XOVERLAPPED*
// Todo conteudo local pertence ao unico usuario.
KERNEL_FUNC(__imp__XamContentGetCreator) {
    uint32_t result = kErrorSuccess;
    if (!ctx.r4.u32) {
        result = kErrorInvalidParameter;
    } else {
        std::error_code ec;
        if (!std::filesystem::is_directory(ContentPath(ReadContentData(base, ctx.r4.u32)), ec)) result = kErrorFileNotFound;
    }
    if (result == kErrorSuccess) {
        if (ctx.r5.u32) St32(base, ctx.r5.u32, 1);
        if (ctx.r6.u32) *reinterpret_cast<uint64_t*>(base + ctx.r6.u32) = Endian::Swap64(0xE000000000000001ull);
    }
    ctx.r3.u64 = ctx.r7.u32 ? CompleteOverlapped(base, ctx.r7.u32, result, 0) : result;
}

// r3 = usuario, r4 = tipo de conteudo, r5 = flags, r6 = bytes necessarios (64 bits num registrador),
// r7 = dispositivo*, r8 = XOVERLAPPED*
KERNEL_FUNC(__imp__XamShowDeviceSelectorUI) {
    if (ctx.r7.u32) St32(base, ctx.r7.u32, kHddDeviceId);
    ctx.r3.u64 = CompleteOverlapped(base, ctx.r8.u32, kErrorSuccess, 0);
}

// r3 = dispositivo, r4 = XOVERLAPPED*
KERNEL_FUNC(__imp__XamContentGetDeviceState) {
    ctx.r3.u64 = CompleteOverlapped(base, ctx.r4.u32, kErrorSuccess, 0);
}

// r3 = dispositivo, r4 = XDEVICE_DATA* {tipo u32, id u32, capacidade u64, livre u64, nome UTF-16}
KERNEL_FUNC(__imp__XamContentGetDeviceData) {
    if (ctx.r3.u32 != kHddDeviceId || !ctx.r4.u32) { ctx.r3.u64 = kErrorDeviceNotConnected; return; }
    std::memset(base + ctx.r4.u32, 0, 0x50);
    St32(base, ctx.r4.u32 + 0x00, 1);  // HDD
    St32(base, ctx.r4.u32 + 0x04, kHddDeviceId);
    *reinterpret_cast<uint64_t*>(base + ctx.r4.u32 + 0x08) = Endian::Swap64(8ull << 30);
    *reinterpret_cast<uint64_t*>(base + ctx.r4.u32 + 0x10) = Endian::Swap64(4ull << 30);
    ctx.r3.u64 = kErrorSuccess;
}

// Dialogos do dashboard: concluem como "cancelados/sem acao" sem exibir nada.
// --- XMsg: mensagens para os apps do XAM (XGI 0xFB, XLive 0xFC, XMP 0xFA) ----------------------------
// Sem Live nem player de musica: toda mensagem conclui com sucesso. Mensagens assincronas completam o
// XOVERLAPPED na hora (como CompleteOverlappedImmediate do Xenia); sem isso, quem espera fica preso.
namespace {
    void LogXMsg(const char* kind, uint32_t app, uint32_t message) {
        static std::mutex lock;
        static std::set<uint64_t> seen;
        std::lock_guard<std::mutex> guard(lock);
        if (seen.insert((uint64_t(app) << 32) | message).second) {
            std::fprintf(stderr, "[XAM] %s app=%08X mensagem=%08X\n", kind, app, message);
        }
    }
}

// r3 = app, r4 = mensagem, r5 = parametro 1, r6 = parametro 2
KERNEL_FUNC(__imp__XMsgInProcessCall) {
    (void)base;
    LogXMsg("XMsgInProcessCall", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// r3 = app, r4 = mensagem, r5 = XOVERLAPPED*, r6 = buffer, r7 = tamanho do buffer
KERNEL_FUNC(__imp__XMsgStartIORequest) {
    LogXMsg("XMsgStartIORequest", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(base, ctx.r5.u32, kErrorSuccess, 0) : kErrorSuccess;
}

// r3 = app, r4 = mensagem, r5 = XOVERLAPPED*, r6 = buffer, r7 = tamanho do buffer, r8 = desconhecido
KERNEL_FUNC(__imp__XMsgStartIORequestEx) {
    LogXMsg("XMsgStartIORequestEx", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(base, ctx.r5.u32, kErrorSuccess, 0) : kErrorSuccess;
}

KERNEL_FUNC(__imp__XamShowSigninUI) { ctx.r3.u64 = kErrorSuccess; (void)base; }
KERNEL_FUNC(__imp__XamShowMarketplaceUI) { ctx.r3.u64 = kErrorSuccess; (void)base; }
KERNEL_FUNC(__imp__XamShowDirtyDiscErrorUI) { ctx.r3.u64 = kErrorSuccess; (void)base; }

// --- Perfil do usuario ---------------------------------------------------------------------------

// r3 = title id, r4 = usuario, r5 = quantidade de xuids, r6 = xuids*, r7 = quantidade de configuracoes,
// r8 = ids*, r9 = tamanho do buffer*, r10 = buffer, pilha[0] = XOVERLAPPED*
// Buffer: { u32 quantidade; u32 ponteiro para X_USER_PROFILE_SETTING[quantidade] }, cada setting com 40 bytes:
// from(+0) xuid(+4) setting_id(+16) tipo(+24) valor int32(+32). Todas as configuracoes valem 0.
KERNEL_FUNC(__imp__XamUserReadProfileSettings) {
    constexpr uint32_t kInsufficientBuffer = 122;
    constexpr uint32_t kSettingSize = 40;
    const uint32_t user = ctx.r4.u32;
    const uint32_t count = ctx.r7.u32;
    const uint32_t ids = ctx.r8.u32;
    const uint32_t sizePtr = ctx.r9.u32;
    const uint32_t buffer = ctx.r10.u32;
    const uint32_t overlapped = Endian::ReadBe32(base + ctx.r1.u32 + 0x54);
    if (g_traceKernel) std::fprintf(stderr, "[trace]   perfil: usuario=%u configuracoes=%u buffer=%08X overlapped=%08X\n", user, count, buffer, overlapped);

    if (user >= 4 && user != 0xFE && user != 0xFF) { ctx.r3.u64 = kErrorNoSuchUser; return; }
    const uint32_t needed = 8 + count * kSettingSize;
    const uint32_t capacity = sizePtr ? Endian::ReadBe32(base + sizePtr) : 0;
    if (!buffer || capacity < needed) {
        if (sizePtr) St32(base, sizePtr, needed);
        ctx.r3.u64 = kInsufficientBuffer;
        return;
    }

    std::memset(base + buffer, 0, needed);
    St32(base, buffer + 0, count);
    St32(base, buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry = buffer + 8 + i * kSettingSize;
        St32(base, entry + 0, 2);                                    // from: valor do perfil
        St32(base, entry + 4, user);
        St32(base, entry + 16, Endian::ReadBe32(base + ids + i * 4));  // setting_id
        base[entry + 24] = 1;                                        // tipo: INT32
        St32(base, entry + 32, 0);
    }
    ctx.r3.u64 = CompleteOverlapped(base, overlapped, kErrorSuccess, needed);
}
