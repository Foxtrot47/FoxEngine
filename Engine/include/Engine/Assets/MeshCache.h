#pragma once
#include <cstdint>
#include <string>
#include "Engine/Renderer/Mesh.h"

namespace SE {

enum class MeshCookResult : uint8_t { Cooked, UpToDate, Failed };

// Disk-backed derived-data cache for imported meshes: one block-compressed
// .fxmesh per source, validated against every file the importer read and the
// Assimp version.
// All methods are const and safe to call concurrently. Paths are relative to
// the current working directory (the project root).
class MeshCache
{
public:
    static constexpr const char* k_defaultDirectory = "Cache/Meshes";
    explicit MeshCache(std::string directory = k_defaultDirectory);

    // Cooked data when the entry is current (or the source no longer exists);
    // otherwise Assimp import + store (store failure only logs a warning).
    bool Load(const std::string& sourcePath, MeshSourceData& output,
              const Mesh::ImportProgressCallback& progress = {},
              std::string* error = nullptr, bool* cacheHit = nullptr) const;

    MeshCookResult Cook(const std::string& sourcePath, bool force,
                        std::string* error = nullptr) const;

    std::string EntryPath(const std::string& sourcePath) const;

private:
    std::string m_directory;
};

} // namespace SE
