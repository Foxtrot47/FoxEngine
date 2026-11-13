#include "Engine/Core/ProjectRoot.h"

#include <windows.h>
#include <filesystem>

namespace SE {
namespace {

namespace fs = std::filesystem;

bool HasFile(const fs::path& directory, const char* file)
{
    std::error_code ec;
    return fs::is_regular_file(directory / file, ec);
}

fs::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return {};
    return fs::path(path).parent_path();
}

fs::path FindAbove(fs::path directory, const char* file)
{
    while (!directory.empty())
    {
        if (HasFile(directory, file))
            return directory;
        const fs::path parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
}

} // namespace

const fs::path& GetEngineRoot()
{
    static const fs::path root = [] {
        fs::path found = FindAbove(ExecutableDirectory(), "Engine/Scripts/fox/runtime.lua");
        if (found.empty() && HasFile(FOX_ENGINE_ROOT, "Engine/Scripts/fox/runtime.lua"))
            found = FOX_ENGINE_ROOT;
        return found;
    }();
    return root;
}

fs::path EnginePath(const fs::path& relativePath)
{
    return GetEngineRoot() / relativePath;
}

bool SetProjectRootCwd(const fs::path& project)
{
    if (GetEngineRoot().empty())
        return false;

    std::error_code ec;
    fs::path root;
    if (!project.empty())
    {
        root = fs::absolute(project, ec);
        if (ec)
            return false;
        if (root.filename() == "project.json")
            root = root.parent_path();
    }
    else
    {
        const fs::path executable = ExecutableDirectory();
        if (HasFile(executable, "project.json"))
            root = executable;
        else
        {
            root = FindAbove(fs::current_path(ec), "project.json");
            if (ec)
                return false;
            if (root.empty())
                root = FOX_DEFAULT_PROJECT;
        }
    }
    if (!HasFile(root, "project.json"))
        return false;
    fs::current_path(root, ec);
    return !ec;
}

} // namespace SE
