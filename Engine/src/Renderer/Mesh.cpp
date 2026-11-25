#include "Engine/Renderer/Mesh.h"
#include "Engine/Core/Logger.h"
#include "Renderer/GltfImport.h"
#include <assimp/Importer.hpp>
#include <assimp/DefaultIOSystem.h>
#include <assimp/IOSystem.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/GltfMaterial.h>
#include <assimp/ProgressHandler.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
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

// Records every file the importer opens for reading (side files such as .bin
// buffers and .mtl libraries) so cooked entries can be invalidated on change.
// DefaultIOSystem is final, hence composition.
class DependencyTrackingIOSystem final : public Assimp::IOSystem
{
public:
    explicit DependencyTrackingIOSystem(std::vector<std::string>& files) : m_files(files) {}

    bool Exists(const char* file) const override { return m_inner.Exists(file); }
    char getOsSeparator() const override { return m_inner.getOsSeparator(); }
    void Close(Assimp::IOStream* file) override { m_inner.Close(file); }
    bool ComparePaths(const char* one, const char* second) const override
    {
        return m_inner.ComparePaths(one, second);
    }

    Assimp::IOStream* Open(const char* file, const char* mode) override
    {
        Assimp::IOStream* stream = m_inner.Open(file, mode);
        if (stream && mode && mode[0] == 'r')
            m_files.push_back(std::filesystem::path(file).lexically_normal().generic_string());
        return stream;
    }

private:
    Assimp::DefaultIOSystem m_inner;
    std::vector<std::string>& m_files;
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

// World transforms per mesh index, gathered from the node hierarchy. A mesh
// referenced by several nodes gets one entry per instance.
static void CollectNodeTransforms(const aiNode* node, const aiMatrix4x4& parent,
                                  std::vector<std::vector<aiMatrix4x4>>& out)
{
    const aiMatrix4x4 world = parent * node->mTransformation;
    for (uint32_t i = 0; i < node->mNumMeshes; ++i)
        out[node->mMeshes[i]].push_back(world);
    for (uint32_t i = 0; i < node->mNumChildren; ++i)
        CollectNodeTransforms(node->mChildren[i], world, out);
}

static uint32_t PackColorRGBA8(float r, float g, float b, float a)
{
    const auto toByte = [](float x) -> uint32_t {
        const float clamped = x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    return toByte(r) | (toByte(g) << 8) | (toByte(b) << 16) | (toByte(a) << 24);
}

static MeshVertex ReadVertex(const aiMesh* mesh, uint32_t v,
                             const aiMatrix4x4& world, const aiMatrix3x3& normalMat,
                             bool applyTransform)
{
    aiVector3D pos = mesh->mVertices[v];
    aiVector3D n   = mesh->mNormals    ? mesh->mNormals[v]    : aiVector3D(0, 1, 0);
    aiVector3D t   = mesh->mTangents   ? mesh->mTangents[v]   : aiVector3D(1, 0, 0);
    aiVector3D b   = mesh->mBitangents ? mesh->mBitangents[v] : aiVector3D(0, 1, 0);
    const aiVector3D uv  = mesh->mTextureCoords[0] ? mesh->mTextureCoords[0][v]
                                                   : aiVector3D(0, 0, 0);
    const aiVector3D uv1 = mesh->mTextureCoords[1] ? mesh->mTextureCoords[1][v] : uv;
    const aiVector3D uv2 = mesh->mTextureCoords[2] ? mesh->mTextureCoords[2][v] : uv;
    if (applyTransform)
    {
        pos = world * pos;
        n   = (normalMat * n).NormalizeSafe();
        t   = (normalMat * t).NormalizeSafe();
        b   = (normalMat * b).NormalizeSafe();
    }
    MeshVertex vtx;
    vtx.x = pos.x;  vtx.y = pos.y;  vtx.z = pos.z;
    vtx.nx = n.x;   vtx.ny = n.y;   vtx.nz = n.z;
    vtx.u = uv.x;   vtx.v = uv.y;
    vtx.tx = t.x;   vtx.ty = t.y;   vtx.tz = t.z;
    vtx.bx = b.x;   vtx.by = b.y;   vtx.bz = b.z;
    vtx.u1 = uv1.x; vtx.v1 = uv1.y;
    vtx.u2 = uv2.x; vtx.v2 = uv2.y;
    if (mesh->mColors[0])
    {
        const aiColor4D& c = mesh->mColors[0][v];
        vtx.color = PackColorRGBA8(c.r, c.g, c.b, c.a);
    }
    return vtx;
}

// Appends one Assimp mesh instance to the contiguous source streams. The progress
// callback maps the submesh range onto [0.8, 1.0] of the overall import; returning
// false cancels.
static bool ReadSubMesh(const aiScene* scene, uint32_t m, const aiMatrix4x4& world,
                        const Mesh::ImportProgressCallback& progress,
                        MeshSourceData& output)
{
    const aiMesh* mesh = scene->mMeshes[m];
    const float meshCount = static_cast<float>(scene->mNumMeshes);

    const bool applyTransform = !world.IsIdentity();
    aiMatrix3x3 normalMat(world);
    const float det = normalMat.Determinant();
    if (applyTransform)
        normalMat.Inverse().Transpose();
    const bool flipWinding = det < 0.0f;

    MeshSubMeshSource record;
    record.firstVertex = static_cast<uint32_t>(output.vertices.size());
    record.vertexCount = mesh->mNumVertices;
    for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
    {
        if ((v & 0x3ffu) == 0 && progress &&
            !progress(0.8f + 0.2f * (static_cast<float>(m) +
                      0.5f * static_cast<float>(v) / static_cast<float>(mesh->mNumVertices)) /
                      meshCount))
            return false;

        const MeshVertex vtx = ReadVertex(mesh, v, world, normalMat, applyTransform);
        output.vertices.push_back(vtx);
        output.bounds.Expand({ vtx.x, vtx.y, vtx.z });
    }

    record.firstIndex = static_cast<uint32_t>(output.indices.size());
    for (uint32_t f = 0; f < mesh->mNumFaces; ++f)
    {
        if ((f & 0x3ffu) == 0 && progress &&
            !progress(0.8f + 0.2f * (static_cast<float>(m) + 0.5f +
                      0.5f * static_cast<float>(f) / static_cast<float>(mesh->mNumFaces)) /
                      meshCount))
            return false;

        const aiFace& face = mesh->mFaces[f];
        if (flipWinding && face.mNumIndices == 3)
        {
            output.indices.push_back(face.mIndices[2]);
            output.indices.push_back(face.mIndices[1]);
            output.indices.push_back(face.mIndices[0]);
        }
        else
        {
            for (uint32_t i = 0; i < face.mNumIndices; ++i)
                output.indices.push_back(face.mIndices[i]);
        }
    }
    record.indexCount = static_cast<uint32_t>(output.indices.size() - record.firstIndex);

    if (mesh->mMaterialIndex < scene->mNumMaterials)
        ExtractSubMeshMaterial(scene->mMaterials[mesh->mMaterialIndex], record.material);
    output.subMeshes.push_back(std::move(record));
    return true;
}

std::string Mesh::SourceDirectory(const std::string& path)
{
    const size_t pos = path.find_last_of("/\\");
    return pos != std::string::npos ? path.substr(0, pos + 1) : std::string();
}

static bool ImportAssimp(const char* path, MeshSourceData& output,
                         const Mesh::ImportProgressCallback& progress, std::string* error)
{
    if (progress && !progress(0.0f))
        return false;

    Assimp::Importer importer;
    importer.SetIOHandler(new DependencyTrackingIOSystem(output.sourceFiles));
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

    output.sourceFiles.push_back(std::filesystem::path(path).lexically_normal().generic_string());
    std::sort(output.sourceFiles.begin(), output.sourceFiles.end());
    output.sourceFiles.erase(std::unique(output.sourceFiles.begin(), output.sourceFiles.end()),
                             output.sourceFiles.end());

    // Callers resolve relative texture paths against this directory.
    output.directory = Mesh::SourceDirectory(path);
    output.bounds = AABB{};

    // Bake node placement into vertices (glTF and FBX position meshes via nodes).
    std::vector<std::vector<aiMatrix4x4>> nodeTransforms(scene->mNumMeshes);
    CollectNodeTransforms(scene->mRootNode, aiMatrix4x4(), nodeTransforms);

    uint64_t totalVertices = 0;
    uint64_t totalIndices = 0;
    size_t totalSubMeshes = 0;
    for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
    {
        const size_t instances = (std::max)(size_t(1), nodeTransforms[m].size());
        totalVertices += uint64_t(scene->mMeshes[m]->mNumVertices) * instances;
        totalIndices += uint64_t(scene->mMeshes[m]->mNumFaces) * 3 * instances;
        totalSubMeshes += instances;
    }
    if (totalVertices > UINT32_MAX || totalIndices > UINT32_MAX)
    {
        const char* message = "mesh exceeds 2^32 vertices or indices";
        if (error) *error = message;
        SE_LOG_ERROR("Mesh::Load '%s': %s", path, message);
        return false;
    }
    output.vertices.reserve(static_cast<size_t>(totalVertices));
    output.indices.reserve(static_cast<size_t>(totalIndices));
    output.subMeshes.reserve(totalSubMeshes);

    for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
    {
        if (progress && !progress(0.8f + 0.2f * static_cast<float>(m) /
                                 static_cast<float>(scene->mNumMeshes)))
            return false;

        // Unreferenced meshes keep identity placement; instanced meshes are duplicated.
        if (nodeTransforms[m].empty())
            nodeTransforms[m].push_back(aiMatrix4x4());

        for (const aiMatrix4x4& world : nodeTransforms[m])
            if (!ReadSubMesh(scene, m, world, progress, output))
                return false;
    }

    if (progress && !progress(1.0f))
        return false;
    SE_LOG_INFO("Mesh imported: '%s' — %zu sub-mesh(es)", path, output.subMeshes.size());
    return true;
}

MeshImporter Mesh::ResolveImporter(const std::string& path, MeshImporter importer)
{
    if (importer != MeshImporter::Auto)
        return importer;
    const size_t dot = path.find_last_of("./\\");
    std::string extension = dot != std::string::npos && path[dot] == '.' ? path.substr(dot) : std::string();
    for (char& c : extension)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return extension == ".gltf" || extension == ".glb" ? MeshImporter::Gltf : MeshImporter::Assimp;
}

bool Mesh::Import(const char* path, MeshSourceData& output,
                  const ImportProgressCallback& progress, std::string* error,
                  MeshImporter importer)
{
    output = {};
    return ResolveImporter(path, importer) == MeshImporter::Gltf
        ? ImportGltf(path, output, progress, error)
        : ImportAssimp(path, output, progress, error);
}

bool Mesh::Upload(ID3D11Device* device, MeshSourceData&& source)
{
    m_subMeshes.clear();
    m_subMeshes.reserve(source.subMeshes.size());
    for (auto& input : source.subMeshes)
    {
        SubMesh sm;
        if (input.vertexCount == 0 || input.indexCount == 0 ||
            !sm.vb.Create(device, source.vertices.data() + input.firstVertex,
                          static_cast<uint32_t>(input.vertexCount * sizeof(MeshVertex)),
                          sizeof(MeshVertex)) ||
            !sm.ib.Create(device, source.indices.data() + input.firstIndex, input.indexCount))
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
