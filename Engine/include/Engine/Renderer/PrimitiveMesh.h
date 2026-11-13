#pragma once
#include <cstdint>
#include <vector>
#include <DirectXMath.h>
#include "Engine/Renderer/Mesh.h"

namespace SE {

// Lit triangle geometry (normals, tangents, UVs) centred on the origin.
void BuildSphereGeometry(float radius, int rings, int segments,
                         std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices);
// size: full extents. Each face is UV-mapped 0..1.
void BuildBoxGeometry(DirectX::XMFLOAT3 size,
                      std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices);

// One untextured submesh with bounds, ready for Mesh::Upload.
MeshSourceData BuildSphereMeshSource(float radius, float roughness);
MeshSourceData BuildBoxMeshSource(DirectX::XMFLOAT3 size, float roughness);

} // namespace SE
