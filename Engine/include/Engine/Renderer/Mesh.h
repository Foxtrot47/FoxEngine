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
// Matches k_meshInputLayout (MeshVertexLayout.h) and Basic.hlsl VSIn.
struct MeshVertex
{
    float x = 0.0f,  y = 0.0f,  z = 0.0f;    // POSITION
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;   // NORMAL
    float u = 0.0f,  v = 0.0f;               // TEXCOORD0
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;   // TANGENT
    float bx = 0.0f, by = 0.0f, bz = 0.0f;   // BINORMAL (bitangent)
    float u1 = 0.0f, v1 = 0.0f;              // TEXCOORD1
    float u2 = 0.0f, v2 = 0.0f;              // TEXCOORD2
    uint32_t color = 0xFFFFFFFFu;            // COLOR0 (RGBA8 UNORM, R in low byte)
};

// Per-submesh material as extracted from the source file.
enum class AlphaMode : uint8_t { Opaque, Cutout, Transparent };

// How the roughness-slot texture (t1) is interpreted:
//  SeparateMR:    t1.r = roughness, metallic map (t7.r) is separate
//  PackedMR:      t1 = (unused, roughness, metallic) — glTF metallic-roughness or ORCA FBX
//  SpecularGloss: t1.rgb = specular color (F0), t1.a = glossiness
enum class MaterialWorkflow : uint8_t { SeparateMR, PackedMR, SpecularGloss };

// One material texture: path relative to GetDirectory() (empty = no texture), the UV
// set it samples (0..2 = TEXCOORD_0..2) and its tiling, applied as uv * scale + offset.
struct TextureSlot
{
    std::string path;
    uint8_t uvSet = 0;
    float scale[2] = { 1, 1 };
    float offset[2] = { 0, 0 };
};

struct SubMeshInfo
{
    TextureSlot albedo, normal, roughness, emissive, occlusion, detailAlbedo, detailNormal;
    float       occlusionStrength = 1.0f;    // ao = lerp(1, occlusion, strength)
    uint8_t     occlusionChannel = 0;        // 0 = R, 1 = G
    float       detailNormalStrength = 1.0f;
    AlphaMode   alphaMode = AlphaMode::Opaque;
    float       alphaCutoff = 0.5f;
    MaterialWorkflow workflow = MaterialWorkflow::SeparateMR;
    float       roughnessFactor = 1.0f;   // scalar roughness for texture-less MR materials
    float       glossinessFactor = 1.0f;  // scalar glossiness for spec/gloss materials
};

// Calls fn(const std::string&) for every non-empty texture path of the material.
template <typename Fn>
void ForEachTexturePath(const SubMeshInfo& info, Fn&& fn)
{
    for (const TextureSlot* slot : { &info.albedo, &info.normal, &info.roughness, &info.emissive,
                                     &info.occlusion, &info.detailAlbedo, &info.detailNormal })
        if (!slot->path.empty())
            fn(slot->path);
}

struct MeshSubMeshSource
{
    uint32_t firstVertex = 0;   // into MeshSourceData::vertices
    uint32_t vertexCount = 0;
    uint32_t firstIndex  = 0;   // into MeshSourceData::indices; index values are submesh-local
    uint32_t indexCount  = 0;
    SubMeshInfo material;
};

struct MeshSourceData
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshSubMeshSource> subMeshes;
    std::vector<std::string> sourceFiles; // every file the importer read (normalized, sorted, unique); cache invalidation inputs
    std::string directory;
    AABB bounds;
};

// Source importer for Mesh::Import. Auto selects Gltf for .gltf/.glb and Assimp otherwise.
enum class MeshImporter : uint8_t { Auto, Assimp, Gltf };

class Mesh
{
public:
    // Called periodically during import with normalized progress. Return false to cancel.
    using ImportProgressCallback = std::function<bool(float)>;

    // Bump `k_meshCacheVersion` (`MeshCache.cpp`) whenever this code changes import
    // output; Assimp upgrades are detected by the cache itself, cgltf upgrades by its
    // pinned version in `ImporterVersion` (`MeshCache.cpp`), which must follow vcpkg.
    static bool Import(const char* path, MeshSourceData& output,
                       const ImportProgressCallback& progress = {},
                       std::string* error = nullptr,
                       MeshImporter importer = MeshImporter::Auto);
    // The importer `Import` uses for `path` (never Auto).
    static MeshImporter ResolveImporter(const std::string& path, MeshImporter importer = MeshImporter::Auto);
    // Directory part of `path` including the trailing separator, or "" if none.
    static std::string SourceDirectory(const std::string& path);
    bool Upload(ID3D11Device* device, MeshSourceData&& source);
    void Draw(ID3D11DeviceContext* ctx) const;
    void DrawSubMesh(ID3D11DeviceContext* ctx, uint32_t index) const;

    uint32_t         GetSubMeshCount() const { return static_cast<uint32_t>(m_subMeshes.size()); }
    const SubMeshInfo& GetSubMeshInfo(uint32_t index) const { return m_subMeshes[index].info; }
    AlphaMode        GetSubMeshAlphaMode(uint32_t index) const { return m_subMeshes[index].info.alphaMode; }
    const std::string& GetDirectory() const { return m_directory; }
    const AABB&      GetBounds() const { return m_bounds; }

private:
    struct SubMesh
    {
        VertexBuffer vb;
        IndexBuffer  ib;
        SubMeshInfo  info;
    };
    std::vector<SubMesh> m_subMeshes;
    std::string          m_directory;
    AABB                 m_bounds;
};

} // namespace SE
