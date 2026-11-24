#include "Engine/Scene/SceneSaver.h"
#include "Engine/Scene/LevelAsset.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Core/Logger.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unordered_set>

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
        { "characterSpawn", desc.physics.characterSpawn },
        { "gravity", desc.physics.gravity }
    };
    settings["toneMapping"] = {
        { "operator", desc.toneMapping.op == 0 ? "Reinhard" : "ACES" },
        { "exposure", desc.toneMapping.exposure }
    };
    settings["colorGrading"] = {
        { "temperature", desc.colorGrading.temperature },
        { "tint", desc.colorGrading.tint },
        { "saturation", desc.colorGrading.saturation },
        { "contrast", desc.colorGrading.contrast },
        { "vignette", desc.colorGrading.vignette },
        { "vignetteFeather", desc.colorGrading.vignetteFeather }
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
    if (desc.shadows.distance > 0.0f)
        settings["shadows"] = { { "distance", desc.shadows.distance } };
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
        { "receiveShadows", mesh.receiveShadows },
        { "collision", mesh.collision },
        { "visible", mesh.visible }
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

ojson WriteScripts(const std::vector<LevelScriptDefinition>& scripts)
{
    ojson out = ojson::array();
    for (const auto& script : scripts)
    {
        ojson item = { { "script", script.script } };
        if (script.properties.is_object() && !script.properties.empty())
            item["properties"] = ojson(script.properties);
        out.push_back(std::move(item));
    }
    return out;
}

// Builds the entities array. Non-zero source IDs are preserved; zero IDs (and
// any repeat of an ID already written, e.g. an entity split into several
// descriptor entries) get fresh sequential IDs above the highest ID in use.
ojson WriteEntities(const SceneDescriptor& desc)
{
    uint64_t nextID = (std::max)(desc.camera.sourceEntityID, desc.sun.sourceEntityID);
    auto observe = [&nextID](const auto& entries) {
        for (const auto& entry : entries)
            nextID = (std::max)(nextID, entry.sourceEntityID);
    };
    observe(desc.meshes);
    observe(desc.pointLights);
    observe(desc.objects);
    observe(desc.particles);
    observe(desc.empties);
    observe(desc.actors);

    std::unordered_set<uint64_t> usedIDs;
    auto allocateID = [&](uint64_t id) {
        if (id == 0 || !usedIDs.insert(id).second)
        {
            id = ++nextID;
            usedIDs.insert(id);
        }
        return id;
    };
    ojson entities = ojson::array();
    auto addEntity = [&](uint64_t id, const std::string& name, ojson components) {
        ojson entity = {
            { "id", FormatEntityID(allocateID(id)) },
            { "name", name },
            { "active", true },
            { "components", std::move(components) }
        };
        entities.push_back(std::move(entity));
    };

    for (const auto& mesh : desc.meshes)
        addEntity(mesh.sourceEntityID, mesh.name, WriteMeshComponents(mesh));
    addEntity(desc.camera.sourceEntityID, desc.camera.name.empty() ? "Camera" : desc.camera.name,
              { { "camera", WriteCamera(desc.camera) } });
    addEntity(desc.sun.sourceEntityID, desc.sun.name.empty() ? "Sun" : desc.sun.name,
              { { "directionalLight", {
                  { "elevation", desc.sun.elevation },
                  { "azimuth", desc.sun.azimuth },
                  { "intensity", desc.sun.intensity },
                  { "color", desc.sun.color }
              } } });
    for (size_t i = 0; i < desc.pointLights.size(); ++i)
    {
        const auto& light = desc.pointLights[i];
        addEntity(light.sourceEntityID,
                  light.name.empty() ? "Point Light " + std::to_string(i + 1) : light.name,
                  { { "pointLight", WritePointLight(light) } });
    }
    for (size_t i = 0; i < desc.objects.size(); ++i)
    {
        const auto& object = desc.objects[i];
        const bool plane = object.type == SceneDescriptor::SceneObject::Type::Plane;
        addEntity(object.sourceEntityID,
                  object.name.empty() ? "Object " + std::to_string(i + 1) : object.name,
                  { { plane ? "planeRenderer" : "sphereRenderer", WriteObject(object) } });
    }
    for (size_t i = 0; i < desc.particles.size(); ++i)
    {
        const auto& emitter = desc.particles[i];
        addEntity(emitter.sourceEntityID,
                  emitter.name.empty() ? "Particle Emitter " + std::to_string(i + 1) : emitter.name,
                  { { "particleEmitter", WriteParticleEmitter(emitter) } });
    }
    for (const auto& empty : desc.empties)
    {
        ojson components = ojson::object();
        components["transform"] = {
            { "position", empty.position },
            { "rotationDegrees", empty.rotation },
            { "scale", empty.scale }
        };
        addEntity(empty.sourceEntityID, empty.name.empty() ? "Empty" : empty.name,
                  std::move(components));
    }
    // Actors: the class path plus only what the level overrides.
    for (const auto& actor : desc.actors)
    {
        ojson components = ojson::object();
        components["transform"] = {
            { "position", actor.position },
            { "rotationDegrees", actor.rotation },
            { "scale", actor.scale }
        };
        if (actor.components.is_object())
            for (const auto& item : actor.components.items())
                if (item.key() != "transform" && item.value().is_object() && !item.value().empty())
                    components[item.key()] = ojson(item.value());
        ojson entity = {
            { "id", FormatEntityID(allocateID(actor.sourceEntityID)) },
            { "name", actor.name.empty() ? "Actor" : actor.name },
            { "active", true },
            { "actor", actor.actorClass },
            { "components", std::move(components) }
        };
        if (actor.properties.is_object() && !actor.properties.empty())
            entity["properties"] = ojson(actor.properties);
        if (!actor.scripts.empty())
            entity["scripts"] = WriteScripts(actor.scripts);
        entities.push_back(std::move(entity));
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
