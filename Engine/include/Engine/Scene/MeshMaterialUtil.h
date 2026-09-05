#pragma once
#include <string>
#include <vector>
#include "Engine/Renderer/ForwardPipeline.h"

namespace SE {

class AssetManager;
class Mesh;

// Resolves a submesh texture reference to an on-disk path. DDS variants are
// preferred; source-format files (png/jpg) are development fallbacks so
// Assimp-imported assets render without a conversion step.
std::string MaterialTexturePath(const std::string& directory, const std::string& raw);

// Builds per-submesh materials for an uploaded mesh, resolving textures from
// the AssetManager cache; missing maps fall back to the 1x1 defaults.
std::vector<ForwardPipeline::SubMat> BuildSubMeshMaterials(AssetManager& assets,
                                                           const Mesh& mesh);

} // namespace SE
