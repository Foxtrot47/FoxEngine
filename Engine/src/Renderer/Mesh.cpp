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

        const aiMesh* mesh = scene->mMeshes[m];

        MeshSubMeshSource source;
        source.vertices.reserve(mesh->mNumVertices);

        for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
        {
            if ((v & 0x3ffu) == 0 && progress &&
                !progress(0.8f + 0.2f * (static_cast<float>(m) +
                          0.5f * static_cast<float>(v) / static_cast<float>(mesh->mNumVertices)) /
                          static_cast<float>(scene->mNumMeshes)))
                return false;

            MeshVertex vtx;
            vtx.x  = mesh->mVertices[v].x;
            vtx.y  = mesh->mVertices[v].y;
            vtx.z  = mesh->mVertices[v].z;
            vtx.nx = mesh->mNormals ? mesh->mNormals[v].x : 0.0f;
            vtx.ny = mesh->mNormals ? mesh->mNormals[v].y : 1.0f;
            vtx.nz = mesh->mNormals ? mesh->mNormals[v].z : 0.0f;
            if (mesh->mTextureCoords[0])
            {
                vtx.u = mesh->mTextureCoords[0][v].x;
                vtx.v = mesh->mTextureCoords[0][v].y;
            }
            else { vtx.u = vtx.v = 0.0f; }
            vtx.tx = mesh->mTangents   ? mesh->mTangents[v].x   : 1.0f;
            vtx.ty = mesh->mTangents   ? mesh->mTangents[v].y   : 0.0f;
            vtx.tz = mesh->mTangents   ? mesh->mTangents[v].z   : 0.0f;
            vtx.bx = mesh->mBitangents ? mesh->mBitangents[v].x : 0.0f;
            vtx.by = mesh->mBitangents ? mesh->mBitangents[v].y : 1.0f;
            vtx.bz = mesh->mBitangents ? mesh->mBitangents[v].z : 0.0f;
            source.vertices.push_back(vtx);
            output.bounds.Expand({ vtx.x, vtx.y, vtx.z });
        }

        source.indices.reserve(mesh->mNumFaces * 3);
        for (uint32_t f = 0; f < mesh->mNumFaces; ++f)
        {
            if ((f & 0x3ffu) == 0 && progress &&
                !progress(0.8f + 0.2f * (static_cast<float>(m) + 0.5f +
                          0.5f * static_cast<float>(f) / static_cast<float>(mesh->mNumFaces)) /
                          static_cast<float>(scene->mNumMeshes)))
                return false;

            const aiFace& face = mesh->mFaces[f];
            for (uint32_t i = 0; i < face.mNumIndices; ++i)
                source.indices.push_back(face.mIndices[i]);
        }

        // Extract material texture paths (glTF PBR slots).
        if (mesh->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
            aiString s;

            auto tryGet = [&](aiTextureType t) -> std::string {
                if (mat->GetTexture(t, 0, &s) == AI_SUCCESS &&
                    s.length > 0 && s.C_Str()[0] != '*')
                    return s.C_Str();
                return {};
            };

            // Base color: prefer PBR slot, fall back to legacy diffuse.
            source.material.albedoPath = tryGet(aiTextureType_BASE_COLOR);
            if (source.material.albedoPath.empty())
                source.material.albedoPath = tryGet(aiTextureType_DIFFUSE);

            // Normal map.
            source.material.normalPath = tryGet(aiTextureType_NORMALS);

            // Roughness / metallic workflow detection from typed Assimp slots only
            // (never UNKNOWN or filename guesses):
            //  - glTF metallic-roughness: Assimp reports the packed texture as both
            //    DIFFUSE_ROUGHNESS and METALNESS (same path) -> G=roughness, B=metallic.
            //  - UE/ORCA FBX exports: the SPECULAR slot carries the same packed
            //    layout (content-verified: R unused, G roughness, B metallic).
            //  - Texture-less materials fall back to the scalar roughness factor.
            source.material.roughnessPath = tryGet(aiTextureType_DIFFUSE_ROUGHNESS);
            if (!source.material.roughnessPath.empty())
            {
                source.material.packedMR =
                    source.material.roughnessPath == tryGet(aiTextureType_METALNESS);
            }
            else
            {
                source.material.roughnessPath = tryGet(aiTextureType_SPECULAR);
                source.material.packedMR = !source.material.roughnessPath.empty();
            }
            ai_real roughnessFactor = 1.0f;
            if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS)
                source.material.roughnessFactor = static_cast<float>(roughnessFactor);

            // Emissive map
            source.material.emissivePath = tryGet(aiTextureType_EMISSIVE);
            if (source.material.emissivePath.empty())
                source.material.emissivePath = tryGet(aiTextureType_EMISSION_COLOR);

            // Alpha mode detection
            aiString alphaStr;
            if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaStr) == AI_SUCCESS)
            {
                if (strcmp(alphaStr.C_Str(), "MASK") == 0)
                    source.material.alphaMode = AlphaMode::Cutout;
                else if (strcmp(alphaStr.C_Str(), "BLEND") == 0)
                    source.material.alphaMode = AlphaMode::Transparent;
            }
            else
            {
                // FBX fallback: if an opacity texture exists or opacity < 1, treat as cutout
                aiString opacityTex;
                float opacity = 1.0f;
                mat->Get(AI_MATKEY_OPACITY, opacity);

                if (mat->GetTexture(aiTextureType_OPACITY, 0, &opacityTex) == AI_SUCCESS)
                    source.material.alphaMode = AlphaMode::Cutout;
                else if (opacity < 0.99f)
                    source.material.alphaMode = AlphaMode::Transparent;
            }

            // Alpha cutoff (glTF ALPHACUTOFF property)
            float cutoff = 0.5f;
            if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS)
                source.material.alphaCutoff = cutoff;
        }

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
        sm.packedMR = input.material.packedMR;
        sm.roughnessFactor = input.material.roughnessFactor;
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
    info.packedMR      = sm.packedMR;
    info.roughnessFactor = sm.roughnessFactor;
    return info;
}

} // namespace SE
