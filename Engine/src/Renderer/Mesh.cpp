#include "Engine/Renderer/Mesh.h"
#include "Engine/Core/Logger.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
#include <assimp/ProgressHandler.hpp>
#include <utility>
#include <vector>

namespace SE {

namespace {

class ResponsiveProgressHandler final : public Assimp::ProgressHandler
{
public:
    explicit ResponsiveProgressHandler(Mesh::ImportProgressCallback callback)
        : m_callback(std::move(callback)) {}

    bool Update(float percentage) override
    {
        return !m_callback || m_callback(percentage * 0.8f);
    }

private:
    Mesh::ImportProgressCallback m_callback;
};

} // anonymous namespace

static std::string GetTexturePath(const aiMaterial* mat, aiTextureType type)
{
    aiString s;
    if (mat->GetTexture(type, 0, &s) == AI_SUCCESS && s.length > 0 && s.C_Str()[0] != '*')
        return s.C_Str();
    return {};
}

// Roughness / metallic / specular workflow detection from typed Assimp slots
// only (never UNKNOWN or filename guesses):
//  - glTF metallic-roughness: Assimp reports the packed texture as both
//    DIFFUSE_ROUGHNESS and METALNESS (same path) -> G=roughness, B=metallic.
//  - glTF spec/gloss (KHR_materials_pbrSpecularGlossiness): Assimp puts the
//    texture in SPECULAR (RGB=specular color, A=gloss) and sets
//    AI_MATKEY_GLOSSINESS_FACTOR, which no other importer emits.
//  - UE/ORCA FBX exports: a SPECULAR slot without a glossiness factor carries the
//    packed metallic-roughness layout (content-verified: R unused, G roughness, B metallic).
//  - Texture-less materials fall back to the scalar roughness/glossiness factors.
static void ResolveRoughnessWorkflow(const aiMaterial* mat, SubMeshInfo& out)
{
    const std::string specular = GetTexturePath(mat, aiTextureType_SPECULAR);
    ai_real glossinessFactor = 1.0f;
    const bool specGloss =
        mat->Get(AI_MATKEY_GLOSSINESS_FACTOR, glossinessFactor) == AI_SUCCESS;
    if (specGloss)
        out.glossinessFactor = static_cast<float>(glossinessFactor);

    out.roughnessPath = GetTexturePath(mat, aiTextureType_DIFFUSE_ROUGHNESS);
    if (!out.roughnessPath.empty())
    {
        if (out.roughnessPath == GetTexturePath(mat, aiTextureType_METALNESS))
            out.workflow = MaterialWorkflow::PackedMR;
    }
    else if (specGloss && !specular.empty())
    {
        out.roughnessPath = specular;
        out.workflow = MaterialWorkflow::SpecularGloss;
    }
    else if (specGloss)
    {
        // Texture-less spec/gloss: approximate with the scalar MR path
        // (specular color constants are not carried through the pipeline).
        out.roughnessFactor = 1.0f - out.glossinessFactor;
    }
    else if (!specular.empty())
    {
        out.roughnessPath = specular;
        out.workflow = MaterialWorkflow::PackedMR;
    }

    ai_real roughnessFactor = 1.0f;
    if (!specGloss && mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS)
        out.roughnessFactor = static_cast<float>(roughnessFactor);
}

static void DetectAlphaMode(const aiMaterial* mat, SubMeshInfo& out)
{
    aiString alphaStr;
    if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaStr) == AI_SUCCESS)
    {
        if (strcmp(alphaStr.C_Str(), "MASK") == 0)
            out.alphaMode = AlphaMode::Cutout;
        else if (strcmp(alphaStr.C_Str(), "BLEND") == 0)
            out.alphaMode = AlphaMode::Transparent;
    }
    else
    {
        // FBX fallback: if an opacity texture exists or opacity < 1, treat as cutout
        aiString opacityTex;
        float opacity = 1.0f;
        mat->Get(AI_MATKEY_OPACITY, opacity);

        if (mat->GetTexture(aiTextureType_OPACITY, 0, &opacityTex) == AI_SUCCESS)
            out.alphaMode = AlphaMode::Cutout;
        else if (opacity < 0.99f)
            out.alphaMode = AlphaMode::Transparent;
    }

    float cutoff = 0.5f;
    if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS)
        out.alphaCutoff = cutoff;
}

static void ExtractSubMeshMaterial(const aiMaterial* mat, SubMeshInfo& out)
{
    // Base color: prefer PBR slot, fall back to legacy diffuse.
    out.albedoPath = GetTexturePath(mat, aiTextureType_BASE_COLOR);
    if (out.albedoPath.empty())
        out.albedoPath = GetTexturePath(mat, aiTextureType_DIFFUSE);

    out.normalPath = GetTexturePath(mat, aiTextureType_NORMALS);

    ResolveRoughnessWorkflow(mat, out);

    out.emissivePath = GetTexturePath(mat, aiTextureType_EMISSIVE);
    if (out.emissivePath.empty())
        out.emissivePath = GetTexturePath(mat, aiTextureType_EMISSION_COLOR);

    DetectAlphaMode(mat, out);
}

static MeshVertex ReadVertex(const aiMesh* mesh, uint32_t v)
{
    const aiVector3D pos = mesh->mVertices[v];
    const aiVector3D n   = mesh->mNormals    ? mesh->mNormals[v]    : aiVector3D(0, 1, 0);
    const aiVector3D t   = mesh->mTangents   ? mesh->mTangents[v]   : aiVector3D(1, 0, 0);
    const aiVector3D b   = mesh->mBitangents ? mesh->mBitangents[v] : aiVector3D(0, 1, 0);
    const aiVector3D uv  = mesh->mTextureCoords[0] ? mesh->mTextureCoords[0][v]
                                                   : aiVector3D(0, 0, 0);
    MeshVertex vtx;
    vtx.x = pos.x;  vtx.y = pos.y;  vtx.z = pos.z;
    vtx.nx = n.x;   vtx.ny = n.y;   vtx.nz = n.z;
    vtx.u = uv.x;   vtx.v = uv.y;
    vtx.tx = t.x;   vtx.ty = t.y;   vtx.tz = t.z;
    vtx.bx = b.x;   vtx.by = b.y;   vtx.bz = b.z;
    return vtx;
}

// Reads one Assimp mesh into a submesh source. The progress callback maps the
// submesh range onto [0.8, 1.0] of the overall import; returning false cancels.
static bool ReadSubMesh(const aiScene* scene, uint32_t m,
                        const Mesh::ImportProgressCallback& progress,
                        MeshSubMeshSource& out, AABB& bounds)
{
    const aiMesh* mesh = scene->mMeshes[m];
    const float meshCount = static_cast<float>(scene->mNumMeshes);

    out.vertices.reserve(mesh->mNumVertices);
    for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
    {
        if ((v & 0x3ffu) == 0 && progress &&
            !progress(0.8f + 0.2f * (static_cast<float>(m) +
                      0.5f * static_cast<float>(v) / static_cast<float>(mesh->mNumVertices)) /
                      meshCount))
            return false;

        const MeshVertex vtx = ReadVertex(mesh, v);
        out.vertices.push_back(vtx);
        bounds.Expand({ vtx.x, vtx.y, vtx.z });
    }

    out.indices.reserve(mesh->mNumFaces * 3);
    for (uint32_t f = 0; f < mesh->mNumFaces; ++f)
    {
        if ((f & 0x3ffu) == 0 && progress &&
            !progress(0.8f + 0.2f * (static_cast<float>(m) + 0.5f +
                      0.5f * static_cast<float>(f) / static_cast<float>(mesh->mNumFaces)) /
                      meshCount))
            return false;

        const aiFace& face = mesh->mFaces[f];
        for (uint32_t i = 0; i < face.mNumIndices; ++i)
            out.indices.push_back(face.mIndices[i]);
    }

    if (mesh->mMaterialIndex < scene->mNumMaterials)
        ExtractSubMeshMaterial(scene->mMaterials[mesh->mMaterialIndex], out.material);
    return true;
}

bool Mesh::Import(const char* path, MeshSourceData& output,
                  const ImportProgressCallback& progress, std::string* error)
{
    output = {};
    if (progress && !progress(0.0f))
        return false;

    Assimp::Importer importer;
    if (progress)
        importer.SetProgressHandler(new ResponsiveProgressHandler(progress));

    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate          |
        aiProcess_GenSmoothNormals     |
        aiProcess_ConvertToLeftHanded  |
        aiProcess_FlipUVs              |
        aiProcess_JoinIdenticalVertices |
        aiProcess_CalcTangentSpace);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
    {
        if (error) *error = importer.GetErrorString();
        SE_LOG_ERROR("Mesh::Load '%s': %s", path, importer.GetErrorString());
        return false;
    }

    // Extract directory so callers can resolve relative texture paths.
    {
        std::string p(path);
        auto pos = p.find_last_of("/\\");
        output.directory = (pos != std::string::npos) ? p.substr(0, pos + 1) : "";
    }

    output.subMeshes.reserve(scene->mNumMeshes);
    output.bounds = AABB{};

    for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
    {
        if (progress && !progress(0.8f + 0.2f * static_cast<float>(m) /
                                 static_cast<float>(scene->mNumMeshes)))
            return false;

        MeshSubMeshSource source;
        if (!ReadSubMesh(scene, m, progress, source, output.bounds))
            return false;

        output.subMeshes.push_back(std::move(source));
    }

    if (progress && !progress(1.0f))
        return false;
    SE_LOG_INFO("Mesh imported: '%s' — %zu sub-mesh(es)", path, output.subMeshes.size());
    return true;
}

bool Mesh::Upload(ID3D11Device* device, MeshSourceData&& source)
{
    m_subMeshes.clear();
    m_subMeshes.reserve(source.subMeshes.size());
    for (auto& input : source.subMeshes)
    {
        SubMesh sm;
        if (input.vertices.empty() || input.indices.empty() ||
            !sm.vb.Create(device, input.vertices.data(),
                          static_cast<uint32_t>(input.vertices.size() * sizeof(MeshVertex)),
                          sizeof(MeshVertex)) ||
            !sm.ib.Create(device, input.indices.data(), static_cast<uint32_t>(input.indices.size())))
            return false;
        sm.albedoPath = std::move(input.material.albedoPath);
        sm.normalPath = std::move(input.material.normalPath);
        sm.roughnessPath = std::move(input.material.roughnessPath);
        sm.emissivePath = std::move(input.material.emissivePath);
        sm.alphaMode = input.material.alphaMode;
        sm.alphaCutoff = input.material.alphaCutoff;
        sm.workflow = input.material.workflow;
        sm.roughnessFactor = input.material.roughnessFactor;
        sm.glossinessFactor = input.material.glossinessFactor;
        m_subMeshes.push_back(std::move(sm));
    }
    m_directory = std::move(source.directory);
    m_bounds = source.bounds;
    return true;
}

void Mesh::Draw(ID3D11DeviceContext* ctx) const
{
    for (const auto& sm : m_subMeshes)
    {
        sm.vb.Bind(ctx);
        sm.ib.Bind(ctx);
        ctx->DrawIndexed(sm.ib.GetCount(), 0, 0);
    }
}

void Mesh::DrawSubMesh(ID3D11DeviceContext* ctx, uint32_t index) const
{
    const SubMesh& sm = m_subMeshes[index];
    sm.vb.Bind(ctx);
    sm.ib.Bind(ctx);
    ctx->DrawIndexed(sm.ib.GetCount(), 0, 0);
}

SubMeshInfo Mesh::GetSubMeshInfo(uint32_t index) const
{
    const SubMesh& sm = m_subMeshes[index];
    SubMeshInfo info;
    info.albedoPath    = sm.albedoPath;
    info.normalPath    = sm.normalPath;
    info.roughnessPath = sm.roughnessPath;
    info.emissivePath  = sm.emissivePath;
    info.alphaMode     = sm.alphaMode;
    info.alphaCutoff   = sm.alphaCutoff;
    info.workflow      = sm.workflow;
    info.roughnessFactor = sm.roughnessFactor;
    info.glossinessFactor = sm.glossinessFactor;
    return info;
}

} // namespace SE
