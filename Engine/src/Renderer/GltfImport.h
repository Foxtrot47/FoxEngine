#pragma once
#include "Engine/Renderer/Mesh.h"
#include <string>

namespace SE {

// Native glTF 2.0 (.gltf/.glb) reader. Output matches Mesh::Import's Assimp path
// (left-handed, glTF UVs as authored, node transforms baked, one submesh per primitive instance).
bool ImportGltf(const char* path, MeshSourceData& out,
                const Mesh::ImportProgressCallback& progress, std::string* error);

} // namespace SE
