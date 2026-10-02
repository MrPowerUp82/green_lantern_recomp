#pragma once

#include <string>
#include <filesystem>

namespace Platform {
    class VFS {
    public:
        static void Initialize(const std::filesystem::path& gameRoot);
        static std::filesystem::path ResolvePath(const std::string& guestPath);

    private:
        static std::filesystem::path s_gameRoot;
    };
}
