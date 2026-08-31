#pragma once
#include <d3d11.h>
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include "Engine/Renderer/VertexBuffer.h"
#include "Engine/Renderer/IndexBuffer.h"
#include "Engine/Physics/AABB.h"

namespace SE {

// Vertex layout used for all mesh geometry.
// Matches the POSITION/NORMAL/TEXCOORD input layout in Basic.hlsl.
struct MeshVertex
{
    float x,  y,  z;    // POSITION
    float nx, ny, nz;   // NORMAL
    float u,  v;        // TEXCOORD
    float tx, ty, tz;   // TANGENT
    float bx, by, bz;   // BINORMAL (bitangent)
};

// Per-submesh texture paths as extracted from the source file's material.
// Paths are relative to GetDirectory(). Empty string = no texture assigned.
enum class AlphaMode : uint8_t { Opaque, Cutout, Transparent };

// How the roughness-slot texture (t1) is interpreted:
//  SeparateMR:    t1.r = roughness, metallic map (t7.r) is separate
//  PackedMR:      t1 = (unused, roughness, metallic) — glTF metallic-roughness or ORCA FBX
//  SpecularGloss: t1.rgb = specular color (F0), t1.a = glossiness
enum class MaterialWorkflow : uint8_t { SeparateMR, PackedMR, SpecularGloss };

struct SubMeshInfo
{
    std::string albedoPath;
    std::string normalPath;
    std::string roughnessPath;
    std::string emissivePath;
    AlphaMode   alphaMode = AlphaMode::Opaque;
    float       alphaCutoff = 0.5f;
    MaterialWorkflow workflow = MaterialWorkflow::SeparateMR;
    float       roughnessFactor = 1.0f;   // scalar roughness for texture-less MR materials
    float       glossinessFactor = 1.0f;  // scalar glossiness for spec/gloss materials
};

struct MeshSubMeshSource
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    SubMeshInfo material;
};

struct MeshSourceData
{
    std::vector<MeshSubMeshSource> subMeshes;
    std::string directory;
    AABB bounds;
};

class Mesh
{
public:
    // Called periodically during import with normalized progress. Return false to cancel.
    using ImportProgressCallback = std::function<bool(float)>;

    static bool Import(const char* path, MeshSourceData& output,
                       const ImportProgressCallback& progress = {},
                       std::string* error = nullptr);
    bool Upload(ID3D11Device* device, MeshSourceData&& source);
    void Draw(ID3D11DeviceContext* ctx) const;
    void DrawSubMesh(ID3D11DeviceContext* ctx, uint32_t index) const;

    uint32_t         GetSubMeshCount() const { return static_cast<uint32_t>(m_subMeshes.size()); }
    SubMeshInfo      GetSubMeshInfo(uint32_t index) const;
    const std::string& GetDirectory() const { return m_directory; }
    const AABB&      GetBounds() const { return m_bounds; }

private:
    struct SubMesh
    {
        VertexBuffer vb;
        IndexBuffer  ib;
        std::string  albedoPath;
        std::string  normalPath;
        std::string  roughnessPath;
        std::string  emissivePath;
        AlphaMode    alphaMode = AlphaMode::Opaque;
        float        alphaCutoff = 0.5f;
        MaterialWorkflow workflow = MaterialWorkflow::SeparateMR;
        float        roughnessFactor = 1.0f;
        float        glossinessFactor = 1.0f;
    };
    std::vector<SubMesh> m_subMeshes;
    std::string          m_directory;
    AABB                 m_bounds;
};

} // namespace SE
