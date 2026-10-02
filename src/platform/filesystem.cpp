#include "filesystem.h"
#include <iostream>
#include <algorithm>

namespace Platform {
    std::filesystem::path VFS::s_gameRoot = "./game";

    void VFS::Initialize(const std::filesystem::path& gameRoot) {
        s_gameRoot = std::filesystem::absolute(gameRoot);
        std::cout << "[VFS] Game Root inicializado em: " << s_gameRoot.string() << std::endl;
    }

    std::filesystem::path VFS::ResolvePath(const std::string& guestPath) {
        std::string normalized = guestPath;
        // Substituir barras invertidas de formato Windows/Xbox
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        // Mapear prefixos clássicos do Xbox 360
        const std::string prefixGame = "game:/";
        const std::string prefixD = "d:/";

        if (normalized.rfind(prefixGame, 0) == 0) {
            normalized = normalized.substr(prefixGame.length());
        } else if (normalized.rfind(prefixD, 0) == 0) {
            normalized = normalized.substr(prefixD.length());
        }

        // Remover barra inicial se houver
        if (!normalized.empty() && normalized.front() == '/') {
            normalized = normalized.substr(1);
        }

        return s_gameRoot / normalized;
    }
}
