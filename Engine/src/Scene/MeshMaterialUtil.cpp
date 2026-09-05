#include "Engine/Scene/MeshMaterialUtil.h"

#include <algorithm>
#include <filesystem>
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Mesh.h"
#include "Engine/Scene/LevelDependencyPlan.h"

namespace SE {
namespace {

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

std::string StemDDS(const std::string& raw)
{
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    const size_t slash = p.rfind('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return (dot == std::string::npos ? name : name.substr(0, dot)) + ".dds";
}

std::string BaseName(const std::string& raw)
{
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

AssetHandle<Texture2D> FindCachedTexture(AssetManager& assets, const std::string& path)
{
    const std::string normalized = LevelDependencyPlan::NormalizePath(path);
    return normalized.empty() ? AssetHandle<Texture2D>{} : assets.FindTexture(Wide(normalized));
}

} // namespace

std::string MaterialTexturePath(const std::string& directory, const std::string& raw)
{
    const std::string candidates[] = {
        LevelDependencyPlan::NormalizePath(directory + "Textures/" + StemDDS(raw)),
        LevelDependencyPlan::NormalizePath(directory + StemDDS(raw)),
        LevelDependencyPlan::NormalizePath(directory + BaseName(raw)),
    };
    std::error_code error;
    for (const auto& candidate : candidates)
        if (std::filesystem::exists(candidate, error)) return candidate;
    return candidates[1];
}

std::vector<ForwardPipeline::SubMat> BuildSubMeshMaterials(AssetManager& assets,
                                                           const Mesh& mesh)
{
    std::vector<ForwardPipeline::SubMat> materials;
    materials.reserve(mesh.GetSubMeshCount());
    for (uint32_t i = 0; i < mesh.GetSubMeshCount(); ++i)
    {
        const auto info = mesh.GetSubMeshInfo(i);
        const auto resolve = [&](const std::string& raw) {
            if (raw.empty()) return AssetHandle<Texture2D>{};
            return FindCachedTexture(assets, MaterialTexturePath(mesh.GetDirectory(), raw));
        };
        ForwardPipeline::SubMat material;
        material.albedo = resolve(info.albedoPath);
        material.normal = resolve(info.normalPath);
        material.roughness = resolve(info.roughnessPath);
        material.emissive = resolve(info.emissivePath);
        material.workflow = material.roughness ? info.workflow
                                               : MaterialWorkflow::SeparateMR;
        material.roughnessScale =
            material.roughness ? 1.0f : info.roughnessFactor;
        material.glossinessFactor = info.glossinessFactor;
        if (!material.albedo) material.albedo = assets.GetDefaultWhite();
        if (!material.normal) material.normal = assets.GetDefaultNormal();
        if (!material.roughness) material.roughness = assets.GetDefaultWhite();
        if (!material.emissive) material.emissive = assets.GetDefaultBlack();
        material.metallic = assets.GetDefaultBlack();
        material.alphaMode = info.alphaMode;
        material.alphaCutoff = info.alphaCutoff;
        materials.push_back(std::move(material));
    }
    return materials;
}

} // namespace SE
