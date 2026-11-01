#include "Engine/Core/ProjectRoot.h"

#include <windows.h>
#include <filesystem>

namespace SE {

bool SetProjectRootCwd()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; ++i)
    {
        if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
        {
            SetCurrentDirectoryW(dir.c_str());
            return true;
        }
        dir = dir.parent_path();
    }
    return false;
}

} // namespace SE
