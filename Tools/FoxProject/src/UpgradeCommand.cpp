#include <cstdio>
#include "Common.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneSaver.h"

namespace FoxProject {

int CommandUpgrade(ToolContext& /*context*/, const std::vector<std::string>& args)
{
    std::vector<std::string> scenes;
    for (const std::string& arg : args)
    {
        if (!arg.empty() && arg[0] == '-')
            return UsageError("upgrade", "unknown option '" + arg + "'");
        scenes.push_back(NormalizePath(arg));
    }
    if (scenes.empty())
        for (const std::string& scene : SE::SceneLoader::ScanSceneDirectory(k_scenesDirectory))
            scenes.push_back(NormalizePath(scene));

    size_t upgraded = 0;
    size_t failed = 0;
    for (const std::string& path : scenes)
    {
        const SE::LevelAssetLoadResult result = SE::LevelAssetLoader::LoadFromFile(path);
        if (!result.Succeeded())
        {
            for (const SE::LevelDiagnostic& diagnostic : result.diagnostics)
                if (diagnostic.severity == SE::LevelDiagnosticSeverity::Error)
                    PrintDiagnostic(Severity::Error, path, diagnostic.path + ": " + diagnostic.message);
            ++failed;
            continue;
        }
        if (!result.asset.convertedFromLegacy)
            continue;
        SE::SceneDescriptor desc;
        if (!SE::SceneLoader::BuildDescriptor(result.asset, desc) || !SE::SceneSaver::SaveToFile(path, desc))
        {
            PrintDiagnostic(Severity::Error, path, "could not be rewritten as FoxLevel version 2");
            ++failed;
            continue;
        }
        std::printf("upgraded %s\n", path.c_str());
        ++upgraded;
    }
    std::printf("upgrade: %zu scene(s) upgraded, %zu failed\n", upgraded, failed);
    return failed ? k_exitFailure : k_exitSuccess;
}

} // namespace FoxProject
