#pragma once
#include <string>
#include <vector>
#include "Engine/Renderer/ForwardPipeline.h"

namespace SE {

class AssetManager;
class Mesh;

// Resolves a submesh texture reference to an on-disk path relative to the mesh
// directory: <dir>/Textures/<stem>.dds, the authored relative path as .dds (glTF
// URIs), <dir>/<stem>.dds, then the authored file and its base name as
// development fallbacks so Assimp-imported assets render without conversion.
std::string MaterialTexturePath(const std::string& directory, const std::string& raw);

// Builds per-submesh materials for an uploaded mesh, resolving textures from
// the AssetManager cache; missing maps fall back to the 1x1 defaults.
std::vector<ForwardPipeline::SubMat> BuildSubMeshMaterials(AssetManager& assets,
                                                           const Mesh& mesh);

} // namespace SE
