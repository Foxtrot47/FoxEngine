#include "Engine/Scene/MeshMaterialUtil.h"

#include <algorithm>
#include <filesystem>
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Mesh.h"
#include "Engine/Scene/LevelDependencyPlan.h"

namespace SE {
namespace {

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

std::string Slashed(const std::string& raw)
{
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string StemDDS(const std::string& raw)
{
    const std::string p = Slashed(raw);
    const size_t slash = p.rfind('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return (dot == std::string::npos ? name : name.substr(0, dot)) + ".dds";
}

// Keeps the authored folder (e.g. glTF "textures_png/a.png" -> "textures_png/a.dds").
std::string RelativeDDS(const std::string& raw)
{
    const std::string p = Slashed(raw);
    const size_t slash = p.rfind('/');
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + StemDDS(p);
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
    // Authored relative paths come first after the Textures/ convention: glTF
    // URIs are relative to the mesh file. The fallback names the expected DDS.
    const std::string fallback = LevelDependencyPlan::NormalizePath(directory + StemDDS(raw));
    const std::string candidates[] = {
        LevelDependencyPlan::NormalizePath(directory + "Textures/" + StemDDS(raw)),
        LevelDependencyPlan::NormalizePath(directory + RelativeDDS(raw)),
        fallback,
        LevelDependencyPlan::NormalizePath(directory + Slashed(raw)),
        LevelDependencyPlan::NormalizePath(directory + BaseName(raw)),
    };
    std::error_code error;
    for (const auto& candidate : candidates)
        if (!candidate.empty() && std::filesystem::exists(candidate, error)) return candidate;
    return fallback;
}

std::vector<ForwardPipeline::SubMat> BuildSubMeshMaterials(AssetManager& assets,
                                                           const Mesh& mesh)
{
    std::vector<ForwardPipeline::SubMat> materials;
    materials.reserve(mesh.GetSubMeshCount());
    for (uint32_t i = 0; i < mesh.GetSubMeshCount(); ++i)
    {
        const SubMeshInfo& info = mesh.GetSubMeshInfo(i);
        const auto resolve = [&](const TextureSlot& slot) {
            if (slot.path.empty()) return AssetHandle<Texture2D>{};
            return FindCachedTexture(assets, MaterialTexturePath(mesh.GetDirectory(), slot.path));
        };
        ForwardPipeline::SubMat material;
        material.albedo = resolve(info.albedo);
        material.normal = resolve(info.normal);
        material.roughness = resolve(info.roughness);
        material.emissive = resolve(info.emissive);
        // Absent occlusion/detail maps stay null: the pipeline binds white occlusion
        // and skips detail sampling (sRGB-ness is chosen at bind time).
        material.occlusion = resolve(info.occlusion);
        material.detailAlbedo = resolve(info.detailAlbedo);
        material.detailNormal = resolve(info.detailNormal);
        const TextureSlot* slots[ForwardPipeline::SlotCount] = {
            &info.albedo, &info.normal, &info.roughness, &info.emissive,
            &info.occlusion, &info.detailAlbedo, &info.detailNormal };
        for (uint32_t s = 0; s < ForwardPipeline::SlotCount; ++s)
        {
            ForwardPipeline::SlotTransform& slot = material.slots[s];
            slot.uvSet = slots[s]->uvSet;
            slot.scale[0] = slots[s]->scale[0];
            slot.scale[1] = slots[s]->scale[1];
            slot.offset[0] = slots[s]->offset[0];
            slot.offset[1] = slots[s]->offset[1];
        }
        material.occlusionStrength = info.occlusionStrength;
        material.occlusionChannel = info.occlusionChannel;
        material.detailAlbedoBlend = info.detailAlbedoBlend;
        material.detailAlbedoStrength = info.detailAlbedoStrength;
        material.detailNormalStrength = info.detailNormalStrength;
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
        material.vertexTint = info.vertexTint;
        if (info.kind == MaterialKind::Terrain)
        {
            // Missing textures stay null: the pipeline binds white albedo/overlay,
            // flat normals and a black splat (layer A only).
            const TerrainMaterial& source = info.terrain;
            ForwardPipeline::TerrainMat& terrain = material.terrain;
            for (uint32_t layer = 0; layer < k_maxTerrainLayers; ++layer)
            {
                terrain.layerAlbedo[layer] = resolve(source.layerAlbedo[layer]);
                terrain.layerNormal[layer] = resolve(source.layerNormal[layer]);
                terrain.layerScale[layer] = source.layerScale[layer];
            }
            terrain.splat = resolve(source.splat);
            terrain.overlay = resolve(source.overlay);
            terrain.layerCount = source.layerCount;
            terrain.albedoAlphaIsRoughness = source.albedoAlphaIsRoughness;
            terrain.splatUVSet = source.splat.uvSet;
            terrain.overlayUVSet = source.overlay.uvSet;
            for (int axis = 0; axis < 2; ++axis)
            {
                terrain.splatUVScale[axis] = source.splat.scale[axis];
                terrain.overlayUVScale[axis] = source.overlay.scale[axis];
            }
            terrain.overlayStrength = source.overlayStrength;
            material.roughnessScale = source.roughnessScale;
            material.kind = MaterialKind::Terrain;
        }
        materials.push_back(std::move(material));
    }
    return materials;
}

} // namespace SE
