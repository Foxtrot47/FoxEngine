#pragma once

#include <string>
#include <vector>
#include "Engine/Scene/LevelAsset.h"

namespace SE {

enum class LevelDependencyKind { Mesh, Texture, ParticleTexture, Environment };

struct LevelDependency
{
    std::string path;
    std::string owner;
    LevelDependencyKind kind = LevelDependencyKind::Texture;
    bool required = false;
};

// A deterministic, deduplicated description of the files a level may need.
// This type deliberately contains no D3D objects and is safe to construct in
// tests or on a worker thread.
struct LevelDependencyPlan
{
    std::vector<LevelDependency> dependencies;

    static std::string NormalizePath(const std::string& path);
    static LevelDependencyPlan Build(const LevelAsset& level);
    // Appends a dependency discovered after Build (actor class components),
    // skipping duplicates.
    void Add(const std::string& path, LevelDependencyKind kind, bool required,
             const std::string& owner);
    const LevelDependency* Find(const std::string& path,
                                LevelDependencyKind kind) const;
};

} // namespace SE
