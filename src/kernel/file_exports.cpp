// Imports de arquivo do xboxkrnl.exe (Nt*File, Ob* simbolicos) sobre o sistema de arquivos do host.
//
// Os caminhos do guest (\Device\Cdrom0\..., game:\..., d:\..., \??\...) sao resolvidos para a pasta
// do jogo (somente leitura na pratica) ou para a pasta de saves/cache ao lado do executavel.

#include "recompiled/ppc_recomp_shared.h"

#include "kernel/kernel_state.h"
#include "kernel/memory.h"
#include "platform/filesystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace Kernel;
namespace fs = std::filesystem;

namespace {
    constexpr uint32_t kFileDirectoryFile = 0x00000001;
    constexpr uint32_t kFileOpened = 1;
    constexpr uint32_t kFileCreated = 2;
    constexpr uint32_t kFileOverwritten = 3;

    uint32_t Ld32(uint8_t* base, uint32_t addr) { return Endian::ReadBe32(base + addr); }
    void St32(uint8_t* base, uint32_t addr, uint32_t value) { Endian::WriteBe32(base + addr, value); }
    void St64(uint8_t* base, uint32_t addr, uint64_t value) { *reinterpret_cast<uint64_t*>(base + addr) = Endian::Swap64(value); }
    uint64_t Ld64(uint8_t* base, uint32_t addr) { return Endian::Swap64(*reinterpret_cast<uint64_t*>(base + addr)); }
    uint16_t Ld16(uint8_t* base, uint32_t addr) { return static_cast<uint16_t>((base[addr] << 8) | base[addr + 1]); }

    class KFile : public KObject {
    public:
        KFile() : KObject(Type::File) {}
        ~KFile() override {
#if defined(_WIN32)
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#endif
        }
        bool Signaled(uint32_t) const override { return true; }

        fs::path path;
        std::string guestName;
        bool isDirectory = false;
        uint64_t position = 0;
        std::mutex io;
#if defined(_WIN32)
        HANDLE handle = INVALID_HANDLE_VALUE;
#endif
        // Enumeracao de diretorio
        bool listed = false;
        std::vector<fs::directory_entry> entries;
        size_t nextEntry = 0;
        std::string pattern;
    };

    std::mutex g_linkLock;
    std::map<std::string, std::string> g_symlinks;  // nome (minusculo) -> destino

    std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // Le o nome de um OBJECT_ATTRIBUTES: { root_directory u32, object_name ANSI_STRING*, attributes u32 }
    std::string ObjectName(uint8_t* base, uint32_t attributesPtr) {
        if (!attributesPtr) return {};
        const uint32_t nameStr = Ld32(base, attributesPtr + 4);
        if (!nameStr) return {};
        const uint16_t length = Ld16(base, nameStr);
        const uint32_t buffer = Ld32(base, nameStr + 4);
        if (!buffer) return {};
        return std::string(reinterpret_cast<const char*>(base + buffer), length);
    }

    std::string AnsiStringArg(uint8_t* base, uint32_t ptr) {
        if (!ptr) return {};
        const uint16_t length = Ld16(base, ptr);
        const uint32_t buffer = Ld32(base, ptr + 4);
        return buffer ? std::string(reinterpret_cast<const char*>(base + buffer), length) : std::string();
    }

    fs::path SaveRoot() {
        static const fs::path root = [] {
            fs::path p = fs::absolute("save");
            std::error_code ec;
            fs::create_directories(p, ec);
            return p;
        }();
        return root;
    }

    // Converte um caminho do guest em um caminho do host. Retorna vazio se o dispositivo for desconhecido.
    std::mutex g_mountLock;
    std::map<std::string, fs::path> g_mounts;  // "save:" (minusculo) -> pasta do host

    fs::path ResolveGuestPath(std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');

        {
            std::lock_guard<std::mutex> lock(g_mountLock);
            const std::string lowered = Lower(name);
            for (const auto& [mount, host] : g_mounts) {
                if (lowered.rfind(mount, 0) != 0) continue;
                std::string rest = name.substr(mount.size());
                while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
                return host / rest;
            }
        }

        // Links simbolicos criados pelo titulo (ObCreateSymbolicLink), aplicados ate estabilizar.
        for (int round = 0; round < 4; round++) {
            std::lock_guard<std::mutex> lock(g_linkLock);
            const std::string lowered = Lower(name);
            bool replaced = false;
            for (const auto& [link, target] : g_symlinks) {
                if (lowered.rfind(link, 0) == 0) {
                    name = target + name.substr(link.size());
                    replaced = true;
                    break;
                }
            }
            if (!replaced) break;
        }

        if (name.rfind("/??/", 0) == 0) name = name.substr(4);
        const std::string lowered = Lower(name);

        struct Mount { const char* prefix; int kind; };  // 0 = jogo, 1 = save, 2 = cache
        static const Mount mounts[] = {
            {"game:", 0}, {"d:", 0}, {"/device/cdrom0", 0}, {"/device/harddisk0/partition1", 1},
            {"/device/harddisk0/partition0", 1}, {"hdd:", 1}, {"e:", 1}, {"cache:", 2},
            {"/device/harddisk0/cache0", 2}, {"/device/harddisk0/cache1", 2},
        };
        for (const Mount& mount : mounts) {
            const size_t length = std::strlen(mount.prefix);
            if (lowered.rfind(mount.prefix, 0) != 0) continue;
            std::string rest = name.substr(length);
            while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
            if (mount.kind == 0) return Platform::VFS::ResolvePath("game:/" + rest);
            fs::path root = SaveRoot() / (mount.kind == 1 ? "hdd" : "cache");
            std::error_code ec;
            fs::create_directories(root, ec);
            return root / rest;
        }
        return {};
    }

    uint64_t ToFileTime(const fs::file_time_type& time) {
        // file_clock usa a mesma epoca do FILETIME no MSVC.
        return static_cast<uint64_t>(time.time_since_epoch().count());
    }

    struct FileTimes {
        uint64_t creation = 0, access = 0, write = 0;
        uint64_t size = 0;
        uint32_t attributes = 0;
        bool exists = false;
    };

    FileTimes QueryPathInfo(const fs::path& path) {
        FileTimes info;
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return info;
        info.exists = true;
        info.creation = (uint64_t(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
        info.access = (uint64_t(data.ftLastAccessTime.dwHighDateTime) << 32) | data.ftLastAccessTime.dwLowDateTime;
        info.write = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        info.attributes = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) info.attributes |= 0x01;
#endif
        return info;
    }

    void WriteIoStatus(uint8_t* base, uint32_t ioStatusPtr, uint32_t status, uint32_t information) {
        if (!ioStatusPtr) return;
        St32(base, ioStatusPtr, status);
        St32(base, ioStatusPtr + 4, information);
    }

    void SignalCompletionEvent(uint8_t* base, uint32_t eventHandle) {
        if (!eventHandle) return;
        auto object = LookupHandle(eventHandle);
        if (object && object->type == KObject::Type::Event) SetEvent(static_cast<KEvent&>(*object), base);
    }

    std::shared_ptr<KFile> FileFromHandle(uint32_t handle) {
        auto object = LookupHandle(handle);
        if (!object || object->type != KObject::Type::File) return nullptr;
        return std::static_pointer_cast<KFile>(object);
    }

    // Implementacao comum de NtCreateFile/NtOpenFile.
    uint32_t OpenFile(uint8_t* base, uint32_t handlePtr, uint32_t access, uint32_t attributesPtr, uint32_t ioStatusPtr,
                      uint32_t disposition, uint32_t options) {
        const std::string guestName = ObjectName(base, attributesPtr);
        const fs::path path = ResolveGuestPath(guestName);
        if (g_traceKernel) {
            std::fprintf(stderr, "[trace]   open '%s' -> '%s' existe=%d\n", guestName.c_str(), path.string().c_str(),
                         path.empty() ? -1 : (fs::exists(path) ? 1 : 0));
        }
        if (path.empty()) {
            std::cerr << "[Kernel] dispositivo desconhecido: " << guestName << std::endl;
            WriteIoStatus(base, ioStatusPtr, Status::kObjectPathNotFound, 0);
            return Status::kObjectPathNotFound;
        }

        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        const bool isDirectory = exists ? fs::is_directory(path, ec) : (options & kFileDirectoryFile) != 0;
        const bool wantsWrite = (access & (0x40000000u | 0x2u | 0x4u | 0x10000u | 0x120116u)) != 0 && !(options & kFileDirectoryFile);

        // Disposicoes: 0 supersede, 1 open, 2 create, 3 open_if, 4 overwrite, 5 overwrite_if
        uint32_t information = kFileOpened;
        switch (disposition) {
        case 1:
            if (!exists) { WriteIoStatus(base, ioStatusPtr, Status::kObjectNameNotFound, 0); return Status::kObjectNameNotFound; }
            break;
        case 2:
            if (exists) { WriteIoStatus(base, ioStatusPtr, Status::kObjectNameCollision, 0); return Status::kObjectNameCollision; }
            information = kFileCreated;
            break;
        case 3:
            if (!exists) information = kFileCreated;
            break;
        case 4:
            if (!exists) { WriteIoStatus(base, ioStatusPtr, Status::kObjectNameNotFound, 0); return Status::kObjectNameNotFound; }
            information = kFileOverwritten;
            break;
        default:
            information = exists ? kFileOverwritten : kFileCreated;
            break;
        }

        auto file = std::make_shared<KFile>();
        file->path = path;
        file->guestName = guestName;
        file->isDirectory = isDirectory;

        if (isDirectory) {
            if (!exists) fs::create_directories(path, ec);
        } else {
#if defined(_WIN32)
            if (wantsWrite) fs::create_directories(path.parent_path(), ec);
            DWORD winAccess = GENERIC_READ | (wantsWrite ? GENERIC_WRITE : 0);
            DWORD winDisposition = OPEN_EXISTING;
            switch (disposition) {
            case 1: winDisposition = OPEN_EXISTING; break;
            case 2: winDisposition = CREATE_NEW; break;
            case 3: winDisposition = OPEN_ALWAYS; break;
            case 4: winDisposition = TRUNCATE_EXISTING; break;
            default: winDisposition = CREATE_ALWAYS; break;
            }
            file->handle = CreateFileW(path.c_str(), winAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, winDisposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                const uint32_t status = error == ERROR_ACCESS_DENIED ? Status::kAccessDenied
                                      : error == ERROR_PATH_NOT_FOUND ? Status::kObjectPathNotFound
                                      : Status::kObjectNameNotFound;
                WriteIoStatus(base, ioStatusPtr, status, 0);
                return status;
            }
#endif
        }

        const uint32_t handle = AddHandle(file);
        if (handlePtr) St32(base, handlePtr, handle);
        WriteIoStatus(base, ioStatusPtr, Status::kSuccess, information);
        return Status::kSuccess;
    }

    uint32_t ReadWrite(uint8_t* base, PPCContext& ctx, bool write, uint32_t bufferPtr, uint32_t length, uint32_t offsetPtr) {
        auto file = FileFromHandle(ctx.r3.u32);
        if (!file || file->isDirectory) return Status::kInvalidHandle;

        uint64_t offset;
        {
            std::lock_guard<std::mutex> lock(file->io);
            offset = file->position;
        }
        if (offsetPtr) {
            const uint64_t requested = Ld64(base, offsetPtr);
            if (requested != 0xFFFFFFFFFFFFFFFFull) offset = requested;
        }

        uint32_t transferred = 0;
#if defined(_WIN32)
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD done = 0;
        BOOL ok = write ? WriteFile(file->handle, base + bufferPtr, length, &done, &overlapped)
                        : ReadFile(file->handle, base + bufferPtr, length, &done, &overlapped);
        if (!ok && GetLastError() != ERROR_HANDLE_EOF) {
            WriteIoStatus(base, ctx.r7.u32, Status::kUnsuccessful, 0);
            return Status::kUnsuccessful;
        }
        transferred = done;
#endif
        {
            std::lock_guard<std::mutex> lock(file->io);
            file->position = offset + transferred;
        }
        if (!write && transferred == 0 && length > 0) {
            WriteIoStatus(base, ctx.r7.u32, Status::kEndOfFile, 0);
            SignalCompletionEvent(base, ctx.r4.u32);
            return Status::kEndOfFile;
        }
        WriteIoStatus(base, ctx.r7.u32, Status::kSuccess, transferred);
        SignalCompletionEvent(base, ctx.r4.u32);
        return Status::kSuccess;
    }

    // Leitura/escrita espalhada em paginas de 4 KB (FILE_SEGMENT_ELEMENT = u64 big-endian).
    uint32_t ScatterGather(uint8_t* base, PPCContext& ctx, bool write) {
        auto file = FileFromHandle(ctx.r3.u32);
        if (!file || file->isDirectory) return Status::kInvalidHandle;

        const uint32_t segments = ctx.r8.u32;
        const uint32_t length = ctx.r9.u32;
        uint64_t offset = file->position;
        if (ctx.r10.u32) {
            const uint64_t requested = Ld64(base, ctx.r10.u32);
            if (requested != 0xFFFFFFFFFFFFFFFFull) offset = requested;
        }

        uint32_t total = 0;
        constexpr uint32_t kPage = 0x1000;
#if defined(_WIN32)
        for (uint32_t done = 0; done < length; done += kPage) {
            const uint32_t chunk = std::min(kPage, length - done);
            const uint32_t target = static_cast<uint32_t>(Ld64(base, segments + (done / kPage) * 8));
            OVERLAPPED overlapped{};
            const uint64_t at = offset + done;
            overlapped.Offset = static_cast<DWORD>(at);
            overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD transferred = 0;
            BOOL ok = write ? WriteFile(file->handle, base + target, chunk, &transferred, &overlapped)
                            : ReadFile(file->handle, base + target, chunk, &transferred, &overlapped);
            if (!ok && GetLastError() != ERROR_HANDLE_EOF) break;
            total += transferred;
            if (transferred < chunk) break;
        }
#endif
        {
            std::lock_guard<std::mutex> lock(file->io);
            file->position = offset + total;
        }
        if (!write && total == 0 && length > 0) {
            WriteIoStatus(base, ctx.r7.u32, Status::kEndOfFile, 0);
            SignalCompletionEvent(base, ctx.r4.u32);
            return Status::kEndOfFile;
        }
        WriteIoStatus(base, ctx.r7.u32, Status::kSuccess, total);
        SignalCompletionEvent(base, ctx.r4.u32);
        return Status::kSuccess;
    }

    bool WildcardMatch(const char* pattern, const char* text) {
        if (*pattern == '\0') return *text == '\0';
        if (*pattern == '*') return WildcardMatch(pattern + 1, text) || (*text && WildcardMatch(pattern, text + 1));
        if (*text && (*pattern == '?' || std::tolower(static_cast<unsigned char>(*pattern)) == std::tolower(static_cast<unsigned char>(*text))))
            return WildcardMatch(pattern + 1, text + 1);
        return false;
    }
}

// r3 = handle*, r4 = acesso, r5 = OBJECT_ATTRIBUTES*, r6 = IO_STATUS_BLOCK*, r7 = tamanho alocado*,
// r8 = atributos, r9 = compartilhamento, r10 = disposicao, pilha[0] = opcoes
KERNEL_FUNC(__imp__NtCreateFile) {
    const uint32_t options = Ld32(base, ctx.r1.u32 + 0x54);
    ctx.r3.u64 = OpenFile(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}

// r3 = handle*, r4 = acesso, r5 = OBJECT_ATTRIBUTES*, r6 = IO_STATUS_BLOCK*, r7 = compartilhamento, r8 = opcoes
KERNEL_FUNC(__imp__NtOpenFile) {
    ctx.r3.u64 = OpenFile(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

// r3 = handle, r4 = evento, r5 = apc, r6 = contexto, r7 = IO_STATUS_BLOCK*, r8 = buffer, r9 = tamanho, r10 = offset*
KERNEL_FUNC(__imp__NtReadFile) { ctx.r3.u64 = ReadWrite(base, ctx, false, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32); }
KERNEL_FUNC(__imp__NtWriteFile) { ctx.r3.u64 = ReadWrite(base, ctx, true, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32); }

// r3 = handle, r4 = evento, r5 = apc, r6 = contexto, r7 = IO_STATUS_BLOCK*, r8 = segmentos*, r9 = tamanho, r10 = offset*
KERNEL_FUNC(__imp__NtReadFileScatter) { ctx.r3.u64 = ScatterGather(base, ctx, false); }
KERNEL_FUNC(__imp__NtWriteFileGather) { ctx.r3.u64 = ScatterGather(base, ctx, true); }

KERNEL_FUNC(__imp__NtFlushBuffersFile) { WriteIoStatus(base, ctx.r4.u32, Status::kSuccess, 0); ctx.r3.u64 = Status::kSuccess; }
KERNEL_FUNC(__imp__NtDeviceIoControlFile) { ctx.r3.u64 = Status::kInvalidDeviceRequest; (void)base; }

// r3 = handle, r4 = IO_STATUS_BLOCK*, r5 = buffer, r6 = tamanho, r7 = classe
KERNEL_FUNC(__imp__NtQueryInformationFile) {
    auto file = FileFromHandle(ctx.r3.u32);
    if (!file) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    const uint32_t out = ctx.r5.u32;
    const FileTimes info = QueryPathInfo(file->path);

    uint32_t written = 0;
    switch (ctx.r7.u32) {
    case 4:  // FileBasicInformation
        St64(base, out + 0x00, info.creation);
        St64(base, out + 0x08, info.access);
        St64(base, out + 0x10, info.write);
        St64(base, out + 0x18, info.write);
        St32(base, out + 0x20, info.attributes);
        written = 0x28;
        break;
    case 5:  // FileStandardInformation
        St64(base, out + 0x00, (info.size + 0xFFF) & ~0xFFFull);
        St64(base, out + 0x08, info.size);
        St32(base, out + 0x10, 1);
        base[out + 0x14] = 0;
        base[out + 0x15] = file->isDirectory ? 1 : 0;
        written = 0x18;
        break;
    case 6:  // FileInternalInformation
        St64(base, out, reinterpret_cast<uintptr_t>(file.get()));
        written = 8;
        break;
    case 14:  // FilePositionInformation
        St64(base, out, file->position);
        written = 8;
        break;
    case 34:  // FileNetworkOpenInformation
        St64(base, out + 0x00, info.creation);
        St64(base, out + 0x08, info.access);
        St64(base, out + 0x10, info.write);
        St64(base, out + 0x18, info.write);
        St64(base, out + 0x20, (info.size + 0xFFF) & ~0xFFFull);
        St64(base, out + 0x28, info.size);
        St32(base, out + 0x30, info.attributes);
        written = 0x38;
        break;
    case 35:  // FileAttributeTagInformation
        St32(base, out, info.attributes);
        St32(base, out + 4, 0);
        written = 8;
        break;
    default:
        WriteIoStatus(base, ctx.r4.u32, Status::kInvalidParameter, 0);
        ctx.r3.u64 = Status::kInvalidParameter;
        return;
    }
    WriteIoStatus(base, ctx.r4.u32, Status::kSuccess, written);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = IO_STATUS_BLOCK*, r5 = buffer, r6 = tamanho, r7 = classe
KERNEL_FUNC(__imp__NtSetInformationFile) {
    auto file = FileFromHandle(ctx.r3.u32);
    if (!file) { ctx.r3.u64 = Status::kInvalidHandle; return; }
    if (ctx.r7.u32 == 14) {  // FilePositionInformation
        std::lock_guard<std::mutex> lock(file->io);
        file->position = Ld64(base, ctx.r5.u32);
    }
    WriteIoStatus(base, ctx.r4.u32, Status::kSuccess, 0);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = OBJECT_ATTRIBUTES*, r4 = X_FILE_NETWORK_OPEN_INFORMATION*
KERNEL_FUNC(__imp__NtQueryFullAttributesFile) {
    const fs::path path = ResolveGuestPath(ObjectName(base, ctx.r3.u32));
    const FileTimes info = path.empty() ? FileTimes{} : QueryPathInfo(path);
    if (!info.exists) { ctx.r3.u64 = Status::kObjectNameNotFound; return; }
    const uint32_t out = ctx.r4.u32;
    St64(base, out + 0x00, info.creation);
    St64(base, out + 0x08, info.access);
    St64(base, out + 0x10, info.write);
    St64(base, out + 0x18, info.write);
    St64(base, out + 0x20, (info.size + 0xFFF) & ~0xFFFull);
    St64(base, out + 0x28, info.size);
    St32(base, out + 0x30, info.attributes);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = IO_STATUS_BLOCK*, r5 = buffer, r6 = tamanho, r7 = classe
KERNEL_FUNC(__imp__NtQueryVolumeInformationFile) {
    const uint32_t out = ctx.r5.u32;
    uint32_t written = 0;
    switch (ctx.r7.u32) {
    case 1:  // FileFsVolumeInformation
        std::memset(base + out, 0, 0x18);
        St32(base, out + 0x08, 0x12345678);  // serial
        written = 0x18;
        break;
    case 3:  // FileFsSizeInformation: 8 GB livres de 32 GB, setores de 512 B, 8 setores por unidade
        St64(base, out + 0x00, 8ull * 1024 * 1024);
        St64(base, out + 0x08, 2ull * 1024 * 1024);
        St32(base, out + 0x10, 8);
        St32(base, out + 0x14, 512);
        written = 0x18;
        break;
    case 4:  // FileFsDeviceInformation: disco, sem flags
        St32(base, out + 0x00, 7);
        St32(base, out + 0x04, 0);
        written = 8;
        break;
    case 5: {  // FileFsAttributeInformation
        St32(base, out + 0x00, 0x00000003);  // case preserving + case sensitive search
        St32(base, out + 0x04, 255);
        St32(base, out + 0x08, 8);
        const char16_t name[] = u"FATX";
        for (int i = 0; i < 4; i++) { base[out + 0x0C + i * 2] = static_cast<uint8_t>(name[i] >> 8); base[out + 0x0C + i * 2 + 1] = static_cast<uint8_t>(name[i]); }
        written = 0x14;
        break;
    }
    default:
        WriteIoStatus(base, ctx.r4.u32, Status::kInvalidParameter, 0);
        ctx.r3.u64 = Status::kInvalidParameter;
        return;
    }
    WriteIoStatus(base, ctx.r4.u32, Status::kSuccess, written);
    ctx.r3.u64 = Status::kSuccess;
}

// r3 = handle, r4 = evento, r5 = apc, r6 = contexto, r7 = IO_STATUS_BLOCK*, r8 = X_FILE_DIRECTORY_INFORMATION*,
// r9 = tamanho, r10 = padrao (STRING*), pilha[0] = reiniciar
KERNEL_FUNC(__imp__NtQueryDirectoryFile) {
    auto file = FileFromHandle(ctx.r3.u32);
    if (!file || !file->isDirectory) { ctx.r3.u64 = Status::kInvalidHandle; return; }

    const bool restart = Ld32(base, ctx.r1.u32 + 0x54) != 0;
    std::lock_guard<std::mutex> lock(file->io);
    if (!file->listed || restart) {
        file->entries.clear();
        std::error_code ec;
        for (fs::directory_iterator it(file->path, ec), end; !ec && it != end; it.increment(ec)) file->entries.push_back(*it);
        file->nextEntry = 0;
        file->listed = true;
        file->pattern = ctx.r10.u32 ? AnsiStringArg(base, ctx.r10.u32) : std::string();
    }

    while (file->nextEntry < file->entries.size()) {
        const fs::directory_entry& entry = file->entries[file->nextEntry++];
        const std::string name = entry.path().filename().string();
        if (!file->pattern.empty() && file->pattern != "*" && !WildcardMatch(file->pattern.c_str(), name.c_str())) continue;

        const uint32_t out = ctx.r8.u32;
        if (0x40 + name.size() > ctx.r9.u32) { ctx.r3.u64 = Status::kBufferOverflow; return; }
        const FileTimes info = QueryPathInfo(entry.path());
        std::memset(base + out, 0, 0x40);
        St32(base, out + 0x00, 0);  // proxima entrada
        St64(base, out + 0x08, info.creation);
        St64(base, out + 0x10, info.access);
        St64(base, out + 0x18, info.write);
        St64(base, out + 0x20, info.write);
        St64(base, out + 0x28, info.size);
        St64(base, out + 0x30, (info.size + 0xFFF) & ~0xFFFull);
        St32(base, out + 0x38, info.attributes);
        St32(base, out + 0x3C, static_cast<uint32_t>(name.size()));
        std::memcpy(base + out + 0x40, name.data(), name.size());
        WriteIoStatus(base, ctx.r7.u32, Status::kSuccess, 0x40 + static_cast<uint32_t>(name.size()));
        SignalCompletionEvent(base, ctx.r4.u32);
        ctx.r3.u64 = Status::kSuccess;
        return;
    }

    const uint32_t status = file->nextEntry == 0 && !file->entries.empty() ? Status::kNoSuchFile : Status::kNoMoreFiles;
    WriteIoStatus(base, ctx.r7.u32, status, 0);
    ctx.r3.u64 = status;
}

namespace Kernel {
    void MountDevice(const std::string& name, const std::filesystem::path& hostPath) {
        std::string key = Lower(name);
        if (key.empty() || key.back() != ':') key.push_back(':');
        std::error_code ec;
        fs::create_directories(hostPath, ec);
        std::lock_guard<std::mutex> lock(g_mountLock);
        g_mounts[key] = hostPath;
    }

    void UnmountDevice(const std::string& name) {
        std::string key = Lower(name);
        if (key.empty() || key.back() != ':') key.push_back(':');
        std::lock_guard<std::mutex> lock(g_mountLock);
        g_mounts.erase(key);
    }

    std::filesystem::path ContentRoot() { return SaveRoot() / "content"; }
}

// r3 = ANSI_STRING* nome, r4 = ANSI_STRING* destino
KERNEL_FUNC(__imp__ObCreateSymbolicLink) {
    std::string name = AnsiStringArg(base, ctx.r3.u32);
    std::string target = AnsiStringArg(base, ctx.r4.u32);
    std::replace(name.begin(), name.end(), '\\', '/');
    std::replace(target.begin(), target.end(), '\\', '/');
    std::lock_guard<std::mutex> lock(g_linkLock);
    g_symlinks[Lower(name)] = target;
    ctx.r3.u64 = Status::kSuccess;
}

KERNEL_FUNC(__imp__ObDeleteSymbolicLink) {
    std::string name = AnsiStringArg(base, ctx.r3.u32);
    std::replace(name.begin(), name.end(), '\\', '/');
    std::lock_guard<std::mutex> lock(g_linkLock);
    g_symlinks.erase(Lower(name));
    ctx.r3.u64 = Status::kSuccess;
}
