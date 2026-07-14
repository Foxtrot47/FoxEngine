#pragma once

#include <string>
#include <nlohmann/json_fwd.hpp>
#include "Engine/Scene/LevelAsset.h"

namespace SE {

class LevelAssetLoader
{
public:
    static LevelAssetLoadResult LoadFromFile(const std::string& path);
    static LevelAssetLoadResult Parse(const nlohmann::json& root,
                                      const std::string& sourceName = "<memory>");
};

} // namespace SE
