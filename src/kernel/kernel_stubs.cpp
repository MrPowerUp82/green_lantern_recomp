#include "kernel_stubs.h"
#include <iostream>

namespace Kernel {
    struct ExportEntry {
        std::string moduleName;
        uint32_t ordinal;
        std::string name;
        KernelExportFn fn;
    };

    static std::unordered_map<uint64_t, ExportEntry> s_exports;

    static uint64_t MakeKey(const std::string& moduleName, uint32_t ordinal) {
        uint64_t hash = 0;
        for (char c : moduleName) hash = hash * 31 + (c | 0x20);
        return (hash << 32) | ordinal;
    }

    void ExportManager::RegisterOrdinal(const std::string& moduleName, uint32_t ordinal, KernelExportFn fn, const std::string& name) {
        s_exports[MakeKey(moduleName, ordinal)] = ExportEntry{moduleName, ordinal, name, fn};
    }

    void ExportManager::DumpRegistered() {
        std::cout << "[Kernel] Total de exportações registradas: " << s_exports.size() << std::endl;
    }

    void InitializeExports() {
        // Exemplo: Registro de stubs comuns de threading e debug
        ExportManager::RegisterOrdinal("xboxkrnl.exe", 12, [](uint32_t*) -> uint32_t {
            // DbgPrint
            return 0;
        }, "DbgPrint");

        ExportManager::RegisterOrdinal("xboxkrnl.exe", 100, [](uint32_t*) -> uint32_t {
            // KeInitializeEvent
            return 0;
        }, "KeInitializeEvent");

        ExportManager::RegisterOrdinal("xboxkrnl.exe", 101, [](uint32_t*) -> uint32_t {
            // KeSetEvent
            return 0;
        }, "KeSetEvent");

        ExportManager::DumpRegistered();
    }
}
