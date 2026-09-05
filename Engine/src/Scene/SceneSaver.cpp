#include "Engine/Scene/SceneSaver.h"
#include "Engine/Scene/LevelAsset.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Core/Logger.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace SE {

// Insertion-ordered so saved levels stay human-readable and diffable.
using ojson = nlohmann::ordered_json;

namespace {

std::string FormatEntityID(uint64_t id)
{
    char text[17];
    sprintf_s(text, "%016llx", static_cast<unsigned long long>(id));
    return text;
}

ojson WriteEnvironment(const SceneDescriptor& desc)
{
    ojson environment = ojson::object();
    if (!desc.skybox.empty())
        environment["skybox"] = desc.skybox;
    if (!desc.reflectionPanorama.empty())
        environment["reflectionPanorama"] = desc.reflectionPanorama;
    environment["iblIntensity"] = desc.iblIntensity;
    environment["diffuseIBLSaturation"] = desc.diffuseIBLSaturation;
    return environment;
}

ojson WriteSettings(const SceneDescriptor& desc)
{
    ojson settings = ojson::object();
    settings["environment"] = WriteEnvironment(desc);
    settings["physics"] = {
        { "floor", {
            { "min", desc.physics.floor.min },
            { "max", desc.physics.floor.max },
            { "friction", desc.physics.floor.friction },
            { "restitution", desc.physics.floor.restitution }
        } },
        { "ballSpawn", desc.physics.ballSpawn },
        { "ballRadius", desc.physics.ballRadius },
        { "characterSpawn", desc.physics.characterSpawn },
        { "gravity", desc.physics.gravity }
    };
    settings["toneMapping"] = {
        { "operator", desc.toneMapping.op == 0 ? "Reinhard" : "ACES" },
        { "exposure", desc.toneMapping.exposure },
        { "gammaCorrect", desc.toneMapping.gammaCorrect }
    };
    settings["bloom"] = {
        { "enabled", desc.bloom.enabled },
        { "threshold", desc.bloom.threshold },
        { "intensity", desc.bloom.intensity },
        { "scatter", desc.bloom.scatter }
    };
    settings["fog"] = {
        { "enabled", desc.fog.enabled },
        { "density", desc.fog.density },
        { "anisotropy", desc.fog.anisotropy },
        { "scatterIntensity", desc.fog.scatterIntensity },
        { "steps", desc.fog.steps },
        { "maxDistance", desc.fog.maxDistance },
        { "ambientColor", { desc.fog.ambientColor[0], desc.fog.ambientColor[1],
                            desc.fog.ambientColor[2] } },
        { "ambientIntensity", desc.fog.ambientIntensity },
        { "heightFalloff", desc.fog.heightFalloff },
        { "heightBase", desc.fog.heightBase },
        { "noiseStrength", desc.fog.noiseStrength },
        { "noiseScale", desc.fog.noiseScale },
        { "noiseSpeed", desc.fog.noiseSpeed },
        { "nearClearDistance", desc.fog.nearClearDistance },
        { "distanceDensity", desc.fog.distanceDensity }
    };
    return settings;
}

ojson WriteMeshComponents(const SceneDescriptor::MeshEntry& mesh)
{
    ojson components = ojson::object();
    components["transform"] = {
        { "position", mesh.position },
        { "rotationDegrees", mesh.rotation },
        { "scale", mesh.scale }
    };
    components["meshRenderer"] = {
        { "mesh", mesh.path },
        { "alphaCutoff", mesh.alphaCutoff },
        { "castShadows", mesh.castShadows },
        { "receiveShadows", mesh.receiveShadows }
    };
    return components;
}

ojson WriteCamera(const SceneDescriptor::CameraDesc& camera)
{
    return {
        { "eye", camera.eye },
        { "yaw", camera.yaw },
        { "pitch", camera.pitch },
        { "nearZ", camera.nearZ },
        { "farZ", camera.farZ },
        { "primary", camera.primary }
    };
}

ojson WritePointLight(const SceneDescriptor::PointLightDesc& light)
{
    ojson out = {
        { "position", light.position },
        { "color", light.color },
        { "radius", light.radius },
        { "castShadow", light.castShadow },
        { "flicker", light.flicker }
    };
    if (light.flicker)
    {
        out["flickerAmplitude"] = light.flickerAmplitude;
        out["flickerFrequency"] = light.flickerFrequency;
        out["flickerSeed"] = light.flickerSeed;
    }
    return out;
}

ojson WriteObject(const SceneDescriptor::SceneObject& object)
{
    ojson material = ojson::object();
    const std::pair<const char*, const std::string*> paths[] = {
        { "albedo", &object.albedoPath }, { "normal", &object.normalPath },
        { "roughness", &object.roughnessPath }, { "metallic", &object.metallicPath },
        { "emissive", &object.emissivePath }
    };
    for (const auto& [field, path] : paths)
        if (!path->empty())
            material[field] = *path;
    material["emissive_intensity"] = object.emissiveIntensity;
    material["emissive_color"] = object.emissiveColor;
    material["tint"] = object.tint;
    material["reflectionMode"] = object.planarReflection ? "planar" : "environment";
    material["reflectionStrength"] = object.reflectionStrength;
    material["reflectionResolutionScale"] = object.reflectionResolutionScale;
    material["roughnessScale"] = object.roughnessScale;
    material["metallicScale"] = object.metallicScale;

    ojson out = ojson::object();
    out["position"] = object.position;
    if (object.type == SceneDescriptor::SceneObject::Type::Plane)
    {
        out["halfSizeX"] = object.halfSizeX;
        out["halfSizeZ"] = object.halfSizeZ;
    }
    else
    {
        out["radius"] = object.radius;
    }
    out["material"] = std::move(material);
    return out;
}

ojson WriteParticleEmitter(const SceneDescriptor::ParticleEmitterDesc& e)
{
    ojson out = {
        { "position", e.position },
        { "emit_rate", e.emitRate },
        { "max_particles", e.maxParticles },
        { "lifetime_min", e.lifetimeMin },
        { "lifetime_max", e.lifetimeMax },
        { "velocity_min", e.velocityMin },
        { "velocity_max", e.velocityMax },
        { "size_start", e.sizeStart },
        { "size_end", e.sizeEnd },
        { "color_start", e.colorStart },
        { "color_end", e.colorEnd },
        { "gravity", e.gravity },
        { "spawnShape", e.spawnShape == 1 ? "box" : "sphere" },
        { "spawn_radius", e.spawnRadius },
        { "spawnBoxExtents", e.spawnBoxExtents },
        { "atlasColumns", e.atlasColumns },
        { "atlasRows", e.atlasRows },
        { "atlasFrameCount", e.atlasFrameCount },
        { "atlasSpeed", e.atlasSpeed },
        { "textureRotationDegrees", e.textureRotationDegrees },
        { "softDistance", e.softDistance },
        { "aspectRatio", e.aspectRatio },
        { "blendMode", e.blendMode },
        { "drag", e.drag },
        { "shaderMode", e.shaderMode },
        { "emitMode", e.emitMode },
        { "rotationDegrees", e.rotationDegrees },
        { "randomRotation", e.randomRotation },
        { "tracerStretch", e.tracerStretch },
        { "lightingInfluence", e.lightingInfluence }
    };
    if (!e.texture.empty())
        out["texture"] = e.texture;
    return out;
}

// Builds the entities array. Mesh entities keep their source IDs; everything
// else gets fresh sequential IDs above the highest mesh ID.
ojson WriteEntities(const SceneDescriptor& desc)
{
    uint64_t nextID = 0;
    for (const auto& mesh : desc.meshes)
        nextID = (std::max)(nextID, mesh.sourceEntityID);

    ojson entities = ojson::array();
    auto addEntity = [&](uint64_t id, const std::string& name, ojson components) {
        entities.push_back({
            { "id", FormatEntityID(id != 0 ? id : ++nextID) },
            { "name", name },
            { "active", true },
            { "components", std::move(components) }
        });
    };

    for (const auto& mesh : desc.meshes)
        addEntity(mesh.sourceEntityID, mesh.name, WriteMeshComponents(mesh));
    addEntity(0, "Camera", { { "camera", WriteCamera(desc.camera) } });
    addEntity(0, "Sun", { { "directionalLight", {
        { "elevation", desc.sun.elevation },
        { "azimuth", desc.sun.azimuth },
        { "intensity", desc.sun.intensity },
        { "color", desc.sun.color }
    } } });
    for (size_t i = 0; i < desc.pointLights.size(); ++i)
    {
        const std::string& name = desc.pointLights[i].name;
        addEntity(0, name.empty() ? "Point Light " + std::to_string(i + 1) : name,
                  { { "pointLight", WritePointLight(desc.pointLights[i]) } });
    }
    for (size_t i = 0; i < desc.objects.size(); ++i)
    {
        const bool plane = desc.objects[i].type == SceneDescriptor::SceneObject::Type::Plane;
        const std::string& name = desc.objects[i].name;
        addEntity(0, name.empty() ? "Object " + std::to_string(i + 1) : name,
                  { { plane ? "planeRenderer" : "sphereRenderer",
                      WriteObject(desc.objects[i]) } });
    }
    for (size_t i = 0; i < desc.particles.size(); ++i)
    {
        const std::string& name = desc.particles[i].name;
        addEntity(0, name.empty() ? "Particle Emitter " + std::to_string(i + 1) : name,
                  { { "particleEmitter", WriteParticleEmitter(desc.particles[i]) } });
    }
    return entities;
}

} // namespace

std::string SceneSaver::ToJson(const SceneDescriptor& desc)
{
    ojson root = ojson::object();
    root["format"] = "FoxLevel";
    root["version"] = LevelAsset::kCurrentVersion;
    root["name"] = desc.name;
    root["settings"] = WriteSettings(desc);
    root["entities"] = WriteEntities(desc);
    return root.dump(2) + "\n";
}

bool SceneSaver::SaveToFile(const std::string& path, const SceneDescriptor& desc)
{
    const std::string text = ToJson(desc);

    const LevelAssetLoadResult check =
        LevelAssetLoader::Parse(nlohmann::json::parse(text), path);
    if (!check.Succeeded())
    {
        for (const auto& diagnostic : check.diagnostics)
            if (diagnostic.severity == LevelDiagnosticSeverity::Error)
                SE_LOG_ERROR("SceneSaver: %s: %s", diagnostic.path.c_str(),
                             diagnostic.message.c_str());
        SE_LOG_ERROR("SceneSaver: refusing to save '%s' — generated level failed validation",
                     path.c_str());
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open() || !(file << text))
    {
        SE_LOG_ERROR("SceneSaver: failed to write '%s'", path.c_str());
        return false;
    }
    SE_LOG_INFO("SceneSaver: saved '%s' (%s)", desc.name.c_str(), path.c_str());
    return true;
}

} // namespace SE
