#pragma once

#include "types.h"
#include <string>
#include <unordered_map>
#include <functional>

namespace Kernel {
    using KernelExportFn = std::function<uint32_t(uint32_t* args)>;

    class ExportManager {
    public:
        static void RegisterOrdinal(const std::string& moduleName, uint32_t ordinal, KernelExportFn fn, const std::string& name);
        static void DumpRegistered();
    };

    void InitializeExports();
}
