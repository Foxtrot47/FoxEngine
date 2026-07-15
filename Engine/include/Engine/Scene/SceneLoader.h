#pragma once
#include <string>
#include <vector>
#include "Engine/Scene/SceneDescriptor.h"

namespace SE {

struct LevelAsset;

class SceneLoader
{
public:
    // Load a scene descriptor from a JSON file. Returns true on success.
    static bool LoadFromFile(const std::string& path, SceneDescriptor& out);
    static bool BuildDescriptor(const LevelAsset& asset, SceneDescriptor& out);

    // Load a single particle emitter preset as JSON.
    static bool LoadParticlePreset(const std::string& path, SceneDescriptor::ParticleEmitterDesc& out);

    // Scan a directory for .json scene files. Returns list of file paths.
    static std::vector<std::string> ScanSceneDirectory(const std::string& directory);
    static std::vector<std::string> ScanParticlePresetDirectory(const std::string& directory);
};

} // namespace SE
