// Imports XAudio* do xboxkrnl.exe: driver de render de audio.
//
// O jogo registra callbacks (XAudioRegisterRenderDriverClient; ha um cliente por motor de audio:
// FMOD, video, ...). O driver chama cada callback toda vez que precisa de um frame de 256 amostras em
// 6 canais (float big-endian, entrelacado); a callback entrega o frame com XAudioSubmitRenderDriverFrame.
// O host mistura os clientes, reduz 5.1 para estereo e toca por waveOut. Como no Xenia, o id do driver
// devolvido e 0x41550000 | indice do cliente.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"
#include "platform/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

using namespace Kernel;

namespace {
    constexpr uint32_t kFrameSamples = 256;
    constexpr uint32_t kGuestChannels = 6;
    constexpr uint32_t kSampleRate = 48000;
    constexpr int kBuffers = 6;
    constexpr uint32_t kDriverBase = 0x41550000;
    constexpr uint32_t kMaxClients = 8;

    struct Client {
        bool active = false;
        uint32_t callback = 0;
        uint32_t argument = 0;
        int16_t frame[kFrameSamples * 2] = {};  // ultimo frame entregue (estereo)
        bool submitted = false;
        uint64_t callbacks = 0, frames = 0;  // diagnostico (GL_TRACE_AUDIO)
    };

    std::atomic<bool> g_running{false};
    std::thread g_thread;
    std::mutex g_clientsLock;
    Client g_clients[kMaxClients];
    uint64_t g_framesRequested = 0;

#if defined(_WIN32)
    int16_t ToPcm(float value) {
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        return static_cast<int16_t>(value * 32767.0f);
    }

    // frame: 256 x 6 floats big-endian (FL FR C LFE BL BR) -> estereo 16 bits entrelacado
    void ConvertFrame(const uint8_t* frame, int16_t* out) {
        for (uint32_t i = 0; i < kFrameSamples; i++) {
            float c[kGuestChannels];
            for (uint32_t ch = 0; ch < kGuestChannels; ch++) {
                uint32_t bits = Endian::ReadBe32(frame + (i * kGuestChannels + ch) * 4);
                std::memcpy(&c[ch], &bits, sizeof(float));
            }
            constexpr float k = 0.7071f;
            const float left = c[0] + k * c[2] + k * c[4] + 0.5f * c[3];
            const float right = c[1] + k * c[2] + k * c[5] + 0.5f * c[3];
            out[i * 2] = ToPcm(left);
            out[i * 2 + 1] = ToPcm(right);
        }
    }

    // Cliente que esta com a callback em execucao nesta thread (XAudioSubmitRenderDriverFrame o consulta).
    thread_local int t_currentClient = -1;

    void AudioThread(uint8_t* base) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HWAVEOUT device = nullptr;
        if (waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            std::cerr << "[Audio] nao foi possivel abrir o dispositivo de audio; o jogo ficara mudo." << std::endl;
            device = nullptr;
        }

        static int16_t pcm[kBuffers][kFrameSamples * 2];
        WAVEHDR headers[kBuffers] = {};
        for (int i = 0; i < kBuffers; i++) {
            std::memset(pcm[i], 0, sizeof(pcm[i]));
            headers[i].lpData = reinterpret_cast<LPSTR>(pcm[i]);
            headers[i].dwBufferLength = sizeof(pcm[i]);
            if (device) waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR));
            headers[i].dwFlags |= WHDR_DONE;  // livre
        }

        // O jogo termina de preencher a estrutura do cliente logo apos registra-lo; o driver real so chama a
        // callback depois da primeira requisicao do hardware de audio.
        Sleep(100);

        // Contexto proprio para executar as callbacks do guest.
        ThreadInfo& info = CurrentThread();
        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        InitThreadContext(info, ctx, 0x40000);
        Platform::SetCrashContext(&ctx);
        const uint32_t stackTop = static_cast<uint32_t>(ctx.r1.u32);

        int nextBuffer = 0;
        while (g_running) {
            bool queued = false;
            for (int n = 0; n < kBuffers; n++) {
                WAVEHDR& header = headers[nextBuffer];
                if (!(header.dwFlags & WHDR_DONE)) break;

                // Um frame de cada cliente ativo; os que nao entregam nada ficam em silencio.
                int32_t mix[kFrameSamples * 2] = {};
                for (uint32_t c = 0; c < kMaxClients; c++) {
                    uint32_t callback, argument;
                    {
                        std::lock_guard<std::mutex> lock(g_clientsLock);
                        if (!g_clients[c].active) continue;
                        callback = g_clients[c].callback;
                        argument = g_clients[c].argument;
                        g_clients[c].submitted = false;
                    }
                    if (callback < PPC_CODE_BASE || callback >= PPC_CODE_BASE + PPC_CODE_SIZE) continue;
                    PPCFunc* host = PPC_LOOKUP_FUNC(base, callback);
                    if (!host) continue;

                    t_currentClient = static_cast<int>(c);
                    ctx.r1.u64 = stackTop;
                    ctx.r3.u64 = argument;
                    ctx.lr = 0;
                    try { host(ctx, base); } catch (const ThreadExit&) {}
                    t_currentClient = -1;

                    std::lock_guard<std::mutex> lock(g_clientsLock);
                    ++g_clients[c].callbacks;
                    if (g_clients[c].submitted) {
                        ++g_clients[c].frames;
                        for (uint32_t i = 0; i < kFrameSamples * 2; i++) mix[i] += g_clients[c].frame[i];
                    }
                }
                // GL_TRACE_AUDIO: a cada ~5 s, callbacks chamadas e quadros entregues por cliente.
                static const bool traceAudio = std::getenv("GL_TRACE_AUDIO") != nullptr;
                if (traceAudio && g_framesRequested % 940 == 0) {
                    std::lock_guard<std::mutex> lock(g_clientsLock);
                    std::fprintf(stderr, "[audio] quadro %llu:", static_cast<unsigned long long>(g_framesRequested));
                    for (uint32_t c = 0; c < kMaxClients; c++) {
                        if (g_clients[c].active) {
                            std::fprintf(stderr, " cliente%u(cb=%08X) chamadas=%llu entregues=%llu", c, g_clients[c].callback,
                                         static_cast<unsigned long long>(g_clients[c].callbacks),
                                         static_cast<unsigned long long>(g_clients[c].frames));
                        }
                    }
                    std::fprintf(stderr, "\n");
                }
                for (uint32_t i = 0; i < kFrameSamples * 2; i++) {
                    pcm[nextBuffer][i] = static_cast<int16_t>(mix[i] > 32767 ? 32767 : (mix[i] < -32768 ? -32768 : mix[i]));
                }
                ++g_framesRequested;

                if (device) {
                    header.dwFlags &= ~WHDR_DONE;
                    waveOutWrite(device, &header, sizeof(WAVEHDR));
                }
                nextBuffer = (nextBuffer + 1) % kBuffers;
                queued = true;
                if (!device) break;
            }
            if (!queued || !device) WaitForSingleObject(event, device ? 20 : 5);
            else WaitForSingleObject(event, 20);
        }

        if (device) {
            waveOutReset(device);
            for (int i = 0; i < kBuffers; i++) waveOutUnprepareHeader(device, &headers[i], sizeof(WAVEHDR));
            waveOutClose(device);
        }
        CloseHandle(event);
    }
#endif
}

// r3 = X_AUDIO_RENDER_DRIVER_CLIENT* {callback u32, argumento u32}, r4 = id do driver*
KERNEL_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    uint32_t index = kMaxClients;
    {
        std::lock_guard<std::mutex> lock(g_clientsLock);
        for (uint32_t i = 0; i < kMaxClients; i++) {
            if (!g_clients[i].active) { index = i; break; }
        }
        if (index != kMaxClients) {
            g_clients[index].active = true;
            g_clients[index].callback = Endian::ReadBe32(base + ctx.r3.u32);
            // Como o Xenia: a callback recebe o ponteiro de uma celula que guarda o argumento do cliente
            // (o wrapper do jogo faz `lwz r3,0(r3)`).
            const GuestAddr cell = PoolAlloc(4);
            Endian::WriteBe32(base + cell, Endian::ReadBe32(base + ctx.r3.u32 + 4));
            g_clients[index].argument = cell;
            std::memset(g_clients[index].frame, 0, sizeof(g_clients[index].frame));
        }
    }
    if (index == kMaxClients) { ctx.r3.u64 = 0x80070005; return; }
    if (ctx.r4.u32) Endian::WriteBe32(base + ctx.r4.u32, kDriverBase | index);
    if (g_traceKernel || std::getenv("GL_TRACE_AUDIO")) {
        std::fprintf(stderr, "[audio] cliente %u registrado: callback=%08X arg=%08X\n", index, g_clients[index].callback, g_clients[index].argument);
    }
#if defined(_WIN32)
    if (!g_running.exchange(true)) g_thread = std::thread(AudioThread, base);
#endif
    ctx.r3.u64 = 0;
}

// r3 = id do driver
KERNEL_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    (void)base;
    const uint32_t index = ctx.r3.u32 & (kMaxClients - 1);
    std::lock_guard<std::mutex> lock(g_clientsLock);
    g_clients[index].active = false;
    ctx.r3.u64 = 0;
}

// r3 = id do driver, r4 = frame (256 x 6 floats big-endian)
KERNEL_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
#if defined(_WIN32)
    const uint32_t index = ctx.r3.u32 & (kMaxClients - 1);
    if (ctx.r4.u32 && (ctx.r3.u32 & 0xFFFF0000u) == kDriverBase) {
        std::lock_guard<std::mutex> lock(g_clientsLock);
        ConvertFrame(base + ctx.r4.u32, g_clients[index].frame);
        g_clients[index].submitted = true;
    }
#else
    (void)base;
#endif
    ctx.r3.u64 = 0;
}

// r3 = configuracao de alto-falantes*
KERNEL_FUNC(__imp__XAudioGetSpeakerConfig) {
    if (ctx.r3.u32) Endian::WriteBe32(base + ctx.r3.u32, 0x00010001);  // estereo
    ctx.r3.u64 = 0;
}

// r3 = categoria, r4 = volume* (float 1.0)
KERNEL_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
    if (ctx.r4.u32) Endian::WriteBe32(base + ctx.r4.u32, 0x3F800000);
    ctx.r3.u64 = 0;
}

// --- XMA (decodificador de audio em hardware) ---------------------------------------------------
// Como no Xenia: 320 contextos XMA_CONTEXT_DATA de 64 bytes num bloco de memoria fisica, que o jogo le e
// escreve diretamente. Ainda nao ha decodificacao real: uma thread "decodifica" silencio, consumindo os
// buffers de entrada e enchendo o anel de saida no ritmo do audio, para o FMOD nao esperar para sempre.
//
// XMA_CONTEXT_DATA (dwords big-endian, bitfields a partir do bit 0):
//   dw0: pacotes buf0 [0:11], loop_count [12:19], buf0 valido [20], buf1 valido [21],
//        blocos de saida [22:26], offset de escrita da saida [27:31]
//   dw1: pacotes buf1 [0:11], ..., estereo [29], saida valida [31]
//   dw2: offset de leitura da entrada em bits [0:25]
//   dw4: buffer atual [31]
//   dw5/dw6: ponteiros fisicos dos buffers de entrada, dw7: ponteiro fisico da saida
//   dw9: offset de leitura da saida [0:4]
namespace {
    constexpr uint32_t kXmaContextCount = 320;
    constexpr uint32_t kXmaContextSize = 64;
    constexpr uint32_t kXmaBitsPerPacket = 2048 * 8;
    constexpr uint32_t kXmaBitsPerHeader = 32;
    constexpr uint32_t kXmaBytesPerBlock = 256;
    std::mutex g_xmaLock;
    GuestAddr g_xmaContexts = 0;
    bool g_xmaUsed[kXmaContextCount] = {};
    std::atomic<bool> g_xmaThreadStarted{false};

    uint32_t XmaField(uint32_t dword, int shift, int bits) { return (dword >> shift) & ((1u << bits) - 1); }
    uint32_t XmaSetField(uint32_t dword, int shift, int bits, uint32_t value) {
        const uint32_t mask = ((1u << bits) - 1) << shift;
        return (dword & ~mask) | ((value << shift) & mask);
    }
    GuestAddr XmaPhysical(uint32_t address) { return address < 0x20000000 ? address + 0xA0000000 : address; }

    // Um quadro XMA (512 amostras por canal) de silencio. Devolve false se nao houve progresso (sem
    // saida, sem entrada ou anel de saida cheio).
    bool XmaStep(uint8_t* base, GuestAddr context) {
        uint8_t* p = base + context;
        uint32_t dw0 = Endian::ReadBe32(p + 0), dw1 = Endian::ReadBe32(p + 4), dw2 = Endian::ReadBe32(p + 8);
        uint32_t dw4 = Endian::ReadBe32(p + 16);
        const uint32_t dw9 = Endian::ReadBe32(p + 36);
        if (!XmaField(dw1, 31, 1)) return false;  // sem buffer de saida
        uint32_t current = XmaField(dw4, 31, 1);
        const bool valid[2] = { XmaField(dw0, 20, 1) != 0, XmaField(dw0, 21, 1) != 0 };
        if (!valid[current]) {
            if (!valid[current ^ 1]) return false;  // nada para decodificar
            current ^= 1;
            dw2 = XmaSetField(dw2, 0, 26, kXmaBitsPerHeader);
        }

        // Espaco livre no anel de saida (blocos de 256 bytes), deixando um bloco para nao confundir cheio/vazio.
        const uint32_t blocks = XmaField(dw0, 22, 5);
        if (!blocks) return false;
        uint32_t write = XmaField(dw0, 27, 5) % blocks;
        const uint32_t read = XmaField(dw9, 0, 5) % blocks;
        const uint32_t used = (write + blocks - read) % blocks;
        const uint32_t free = blocks - 1 - used;
        const uint32_t frameBlocks = (XmaField(dw1, 29, 1) ? 2048u : 1024u) / kXmaBytesPerBlock;
        if (free < frameBlocks) return false;

        const GuestAddr output = XmaPhysical(Endian::ReadBe32(p + 28));
        for (uint32_t i = 0; i < frameBlocks; ++i) {
            std::memset(base + output + write * kXmaBytesPerBlock, 0, kXmaBytesPerBlock);
            write = (write + 1) % blocks;
        }
        dw0 = XmaSetField(dw0, 27, 5, write);

        // Consome a entrada: GL_XMA_RATE quartos de pacote por quadro. O padrao (4 = um pacote por quadro) foi
        // o que fez a introducao narrada avancar; 1 deixava a narracao longa demais. Empirico: troca por
        // decodificacao real quando houver.
        static const uint32_t rate = std::getenv("GL_XMA_RATE") ? std::max(1, std::atoi(std::getenv("GL_XMA_RATE"))) : 4;
        const uint32_t packets = current ? XmaField(dw1, 0, 12) : XmaField(dw0, 0, 12);
        uint32_t offset = std::max(XmaField(dw2, 0, 26), kXmaBitsPerHeader) + kXmaBitsPerPacket / 4 * rate;
        if (offset >= packets * kXmaBitsPerPacket) {
            dw0 = XmaSetField(dw0, current ? 21 : 20, 1, 0);  // buffer consumido
            current ^= 1;
            offset = kXmaBitsPerHeader;
        }
        dw2 = XmaSetField(dw2, 0, 26, offset);
        dw4 = XmaSetField(dw4, 31, 1, current);

        Endian::WriteBe32(p + 0, dw0);
        Endian::WriteBe32(p + 8, dw2);
        Endian::WriteBe32(p + 16, dw4);
        return true;
    }

    // Como o hardware, decodifica mais rapido que o tempo real: cada passada enche o anel de saida de cada
    // contexto (ou esgota a entrada). Streams tocados em tempo real ficam no ritmo de quem consome a saida;
    // bancos de som decodificados no carregamento terminam logo (antes, 1 quadro a cada 10 ms deixava o
    // loading tao longo quanto a duracao de todo o audio).
    void XmaThread(uint8_t* base) {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(g_xmaLock);
                for (uint32_t i = 0; i < kXmaContextCount; ++i) {
                    if (!g_xmaUsed[i]) continue;
                    for (int frame = 0; frame < 64 && XmaStep(base, g_xmaContexts + i * kXmaContextSize); ++frame) {}
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// r3 = endereco do contexto* ; retorna NTSTATUS
KERNEL_FUNC(__imp__XMACreateContext) {
    if (!g_xmaThreadStarted.exchange(true)) std::thread(XmaThread, base).detach();
    std::lock_guard<std::mutex> lock(g_xmaLock);
    if (!g_xmaContexts) g_xmaContexts = MemoryManager::AllocateAligned(kXmaContextCount * kXmaContextSize, 0x1000, true);
    uint32_t index = kXmaContextCount;
    for (uint32_t i = 0; g_xmaContexts && i < kXmaContextCount; ++i) {
        if (!g_xmaUsed[i]) { index = i; break; }
    }
    if (index == kXmaContextCount) {
        if (ctx.r3.u32) Endian::WriteBe32(base + ctx.r3.u32, 0);
        ctx.r3.u64 = 0xC0000017;  // STATUS_NO_MEMORY
        return;
    }
    g_xmaUsed[index] = true;
    const GuestAddr context = g_xmaContexts + index * kXmaContextSize;
    std::memset(base + context, 0, kXmaContextSize);
    if (ctx.r3.u32) Endian::WriteBe32(base + ctx.r3.u32, context);
    ctx.r3.u64 = 0;
}

// r3 = endereco do contexto
KERNEL_FUNC(__imp__XMAReleaseContext) {
    std::lock_guard<std::mutex> lock(g_xmaLock);
    const GuestAddr context = ctx.r3.u32;
    if (g_xmaContexts && context >= g_xmaContexts && context < g_xmaContexts + kXmaContextCount * kXmaContextSize) {
        const uint32_t index = (context - g_xmaContexts) / kXmaContextSize;
        g_xmaUsed[index] = false;
        std::memset(base + context, 0, kXmaContextSize);
    }
    ctx.r3.u64 = 0;
}

namespace Kernel {
    void ShutdownAudio() {
        g_running = false;
        if (g_thread.joinable()) g_thread.join();
    }
}
