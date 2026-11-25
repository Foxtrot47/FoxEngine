#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Core/Logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace SE {

using json = nlohmann::json;

// Helper to read an array of 3 floats from JSON
static std::array<float, 3> ReadFloat3(const json& j, const std::string& key, std::array<float, 3> def)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return def;
    return { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>() };
}

static std::array<float, 4> ReadFloat4(const json& j, const std::string& key, std::array<float, 4> def)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return def;
    return { j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>(), j[key][3].get<float>() };
}

static void ReadParticleEmitter(const json& p, SceneDescriptor::ParticleEmitterDesc& e)
{
    e.position     = ReadFloat3(p, "position", e.position);
    e.velocityMin  = ReadFloat3(p, "velocity_min", e.velocityMin);
    e.velocityMax  = ReadFloat3(p, "velocity_max", e.velocityMax);
    e.gravity      = ReadFloat3(p, "gravity", e.gravity);
    e.spawnBoxExtents = ReadFloat3(p, "spawnBoxExtents", e.spawnBoxExtents);
    e.colorStart   = ReadFloat4(p, "color_start", e.colorStart);
    e.colorEnd     = ReadFloat4(p, "color_end", e.colorEnd);
    if (p.contains("emit_rate"))      e.emitRate     = p["emit_rate"].get<float>();
    if (p.contains("max_particles"))  e.maxParticles = p["max_particles"].get<int>();
    if (p.contains("lifetime_min"))   e.lifetimeMin  = p["lifetime_min"].get<float>();
    if (p.contains("lifetime_max"))   e.lifetimeMax  = p["lifetime_max"].get<float>();
    if (p.contains("size_start"))     e.sizeStart    = p["size_start"].get<float>();
    if (p.contains("size_end"))       e.sizeEnd      = p["size_end"].get<float>();
    if (p.contains("spawn_radius"))   e.spawnRadius  = p["spawn_radius"].get<float>();
    if (p.contains("spawnShape"))     e.spawnShape = p["spawnShape"].get<std::string>() == "box" ? 1 : 0;
    if (p.contains("texture"))        e.texture      = p["texture"].get<std::string>();
    if (p.contains("atlasColumns"))   e.atlasColumns    = p["atlasColumns"].get<int>();
    if (p.contains("atlasRows"))      e.atlasRows       = p["atlasRows"].get<int>();
    if (p.contains("atlasFrameCount"))e.atlasFrameCount = p["atlasFrameCount"].get<int>();
    if (p.contains("atlasSpeed"))     e.atlasSpeed      = p["atlasSpeed"].get<float>();
    if (p.contains("textureRotationDegrees")) e.textureRotationDegrees = p["textureRotationDegrees"].get<float>();
    if (p.contains("softDistance"))   e.softDistance    = p["softDistance"].get<float>();
    if (p.contains("aspectRatio"))    e.aspectRatio     = p["aspectRatio"].get<float>();
    if (p.contains("blendMode"))      e.blendMode       = p["blendMode"].get<int>();
    if (p.contains("drag"))           e.drag            = p["drag"].get<float>();
    if (p.contains("shaderMode"))     e.shaderMode      = p["shaderMode"].get<int>();
    if (p.contains("emitMode"))       e.emitMode        = p["emitMode"].get<int>();
    if (p.contains("rotationDegrees"))e.rotationDegrees = p["rotationDegrees"].get<float>();
    if (p.contains("randomRotation")) e.randomRotation  = p["randomRotation"].get<bool>();
    if (p.contains("tracerStretch"))  e.tracerStretch   = p["tracerStretch"].get<float>();
    if (p.contains("lightingInfluence")) e.lightingInfluence = p["lightingInfluence"].get<float>();
}

static json BuildSettingsJson(const LevelAsset& asset)
{
    json root = json::object();
    root["name"] = asset.name;

    if (asset.settings.contains("environment") && asset.settings["environment"].is_object())
    {
        const auto& environment = asset.settings["environment"];
        for (const char* field : { "skybox", "reflectionPanorama", "iblIntensity", "diffuseIBLSaturation" })
            if (environment.contains(field))
                root[field] = environment[field];
    }
    for (const char* field : { "physics", "toneMapping", "colorGrading", "bloom", "fog", "shadows", "motionBlur",
                               "reflectionProbes" })
        if (asset.settings.contains(field))
            root[field] = asset.settings[field];

    return root;
}

bool SceneLoader::LoadFromFile(const std::string& path, SceneDescriptor& out)
{
    const LevelAssetLoadResult levelResult = LevelAssetLoader::LoadFromFile(path);
    if (!levelResult.Succeeded())
    {
        for (const auto& diagnostic : levelResult.diagnostics)
            if (diagnostic.severity == LevelDiagnosticSeverity::Error)
                SE_LOG_ERROR("SceneLoader: %s: %s", diagnostic.path.c_str(), diagnostic.message.c_str());
        return false;
    }
    if (!BuildDescriptor(levelResult.asset, out))
        return false;
    SE_LOG_INFO("SceneLoader: Loaded '%s' (%s)", out.name.c_str(), path.c_str());
    return true;
}

bool SceneLoader::BuildDescriptor(const LevelAsset& asset, SceneDescriptor& out)
{
    const json root = BuildSettingsJson(asset);

    out = SceneDescriptor{}; // reset to defaults

    // Name
    if (root.contains("name")) out.name = root["name"].get<std::string>();

    for (const auto& entity : asset.entities)
    {
        if (!entity.active)
            continue;

        if (entity.meshRenderer)
        {
            SceneDescriptor::MeshEntry mesh;
            mesh.sourceEntityID = entity.id;
            mesh.name = entity.name;
            mesh.path = entity.meshRenderer->mesh;
            mesh.alphaCutoff = entity.meshRenderer->alphaCutoff;
            mesh.castShadows = entity.meshRenderer->castShadows;
            mesh.receiveShadows = entity.meshRenderer->receiveShadows;
            mesh.collision = entity.meshRenderer->collision;
            mesh.visible = entity.meshRenderer->visible;
            if (entity.transform)
            {
                mesh.position = entity.transform->position;
                mesh.rotation = entity.transform->rotationDegrees;
                mesh.scale = entity.transform->scale;
            }
            out.meshes.push_back(std::move(mesh));
        }
        if (entity.camera && entity.camera->primary)
        {
            out.camera = *entity.camera;
            out.camera.sourceEntityID = entity.id;
            out.camera.name = entity.name;
        }
        if (entity.directionalLight)
        {
            out.sun = *entity.directionalLight;
            out.sun.sourceEntityID = entity.id;
            out.sun.name = entity.name;
        }
        if (entity.pointLight)
        {
            auto& light = out.pointLights.emplace_back(*entity.pointLight);
            light.sourceEntityID = entity.id;
            light.name = entity.name;
        }
        if (entity.particleEmitter)
        {
            auto& emitter = out.particles.emplace_back(*entity.particleEmitter);
            emitter.sourceEntityID = entity.id;
            emitter.name = entity.name;
        }
        if (entity.primitiveRenderer)
        {
            const auto& source = *entity.primitiveRenderer;
            SceneDescriptor::SceneObject object;
            object.sourceEntityID = entity.id;
            object.name = entity.name;
            object.type = source.type == LevelPrimitiveType::Plane
                ? SceneDescriptor::SceneObject::Type::Plane
                : SceneDescriptor::SceneObject::Type::Sphere;
            object.position = source.position;
            object.radius = source.radius;
            object.halfSizeX = source.halfSizeX;
            object.halfSizeZ = source.halfSizeZ;
            object.albedoPath = source.material.albedo;
            object.normalPath = source.material.normal;
            object.roughnessPath = source.material.roughness;
            object.metallicPath = source.material.metallic;
            object.emissivePath = source.material.emissive;
            object.emissiveIntensity = source.material.emissiveIntensity;
            object.emissiveColor = source.material.emissiveColor;
            object.tint = source.material.tint;
            object.planarReflection = source.material.planarReflection;
            object.reflectionStrength = source.material.reflectionStrength;
            object.reflectionResolutionScale = source.material.reflectionResolutionScale;
            object.roughnessScale = source.material.roughnessScale;
            object.metallicScale = source.material.metallicScale;
            out.objects.push_back(std::move(object));
        }

        if (entity.actor)
        {
            SceneDescriptor::ActorEntry actor;
            actor.sourceEntityID = entity.id;
            actor.name = entity.name;
            actor.actorClass = entity.actor->actorClass;
            actor.components = entity.actor->components;
            actor.properties = entity.actor->properties;
            actor.scripts = entity.scripts;
            if (entity.transform)
            {
                actor.position = entity.transform->position;
                actor.rotation = entity.transform->rotationDegrees;
                actor.scale = entity.transform->scale;
            }
            out.actors.push_back(std::move(actor));
            continue;
        }

        const bool hasKnownComponent = entity.meshRenderer || entity.camera ||
            entity.directionalLight || entity.pointLight || entity.primitiveRenderer ||
            entity.particleEmitter;
        if (!hasKnownComponent)
        {
            SceneDescriptor::EmptyEntity empty;
            empty.sourceEntityID = entity.id;
            empty.name = entity.name;
            if (entity.transform)
            {
                empty.position = entity.transform->position;
                empty.rotation = entity.transform->rotationDegrees;
                empty.scale = entity.transform->scale;
            }
            out.empties.push_back(std::move(empty));
        }
    }

    // Skybox
    if (root.contains("skybox")) out.skybox = root["skybox"].get<std::string>();

    // Optional reflection panorama override.
    if (root.contains("reflectionPanorama")) out.reflectionPanorama = root["reflectionPanorama"].get<std::string>();

    // IBL
    if (root.contains("iblIntensity")) out.iblIntensity = root["iblIntensity"].get<float>();
    if (root.contains("diffuseIBLSaturation"))
        out.diffuseIBLSaturation = root["diffuseIBLSaturation"].get<float>();

    // Physics
    if (root.contains("physics"))
    {
        auto& p = root["physics"];
        if (p.contains("floor"))
        {
            auto& f = p["floor"];
            out.physics.floor.min = ReadFloat3(f, "min", out.physics.floor.min);
            out.physics.floor.max = ReadFloat3(f, "max", out.physics.floor.max);
            if (f.contains("friction"))    out.physics.floor.friction    = f["friction"].get<float>();
            if (f.contains("restitution")) out.physics.floor.restitution = f["restitution"].get<float>();
        }
        out.physics.characterSpawn = ReadFloat3(p, "characterSpawn", out.physics.characterSpawn);
        if (p.contains("gravity"))    out.physics.gravity    = p["gravity"].get<bool>();
    }

    // Tone mapping
    if (root.contains("toneMapping"))
    {
        auto& tm = root["toneMapping"];
        if (tm.contains("operator"))
        {
            std::string op = tm["operator"].get<std::string>();
            out.toneMapping.op = (op == "Reinhard") ? 0 : 1;
        }
        if (tm.contains("exposure"))     out.toneMapping.exposure     = tm["exposure"].get<float>();
    }

    // Color grading
    if (root.contains("colorGrading"))
    {
        auto& cg = root["colorGrading"];
        if (cg.contains("temperature"))     out.colorGrading.temperature     = cg["temperature"].get<float>();
        if (cg.contains("tint"))            out.colorGrading.tint            = cg["tint"].get<float>();
        if (cg.contains("saturation"))      out.colorGrading.saturation      = cg["saturation"].get<float>();
        if (cg.contains("contrast"))        out.colorGrading.contrast        = cg["contrast"].get<float>();
        if (cg.contains("vignette"))        out.colorGrading.vignette        = cg["vignette"].get<float>();
        if (cg.contains("vignetteFeather")) out.colorGrading.vignetteFeather = cg["vignetteFeather"].get<float>();
    }

    // Bloom
    if (root.contains("bloom"))
    {
        auto& b = root["bloom"];
        if (b.contains("enabled"))   out.bloom.enabled   = b["enabled"].get<bool>();
        if (b.contains("threshold")) out.bloom.threshold = b["threshold"].get<float>();
        if (b.contains("intensity")) out.bloom.intensity = b["intensity"].get<float>();
        if (b.contains("scatter"))   out.bloom.scatter   = b["scatter"].get<float>();
    }

    if (root.contains("fog"))
    {
        auto& f = root["fog"];
        if (f.contains("enabled"))          out.fog.enabled          = f["enabled"].get<bool>();
        if (f.contains("density"))          out.fog.density          = f["density"].get<float>();
        if (f.contains("anisotropy"))       out.fog.anisotropy       = f["anisotropy"].get<float>();
        if (f.contains("scatterIntensity")) out.fog.scatterIntensity = f["scatterIntensity"].get<float>();
        if (f.contains("steps"))            out.fog.steps            = f["steps"].get<int>();
        if (f.contains("maxDistance"))      out.fog.maxDistance      = f["maxDistance"].get<float>();
        if (f.contains("ambientColor") && f["ambientColor"].is_array() && f["ambientColor"].size() == 3)
            for (int i = 0; i < 3; ++i) out.fog.ambientColor[i] = f["ambientColor"][i].get<float>();
        if (f.contains("ambientIntensity")) out.fog.ambientIntensity = f["ambientIntensity"].get<float>();
        if (f.contains("heightFalloff"))    out.fog.heightFalloff    = f["heightFalloff"].get<float>();
        if (f.contains("heightBase"))       out.fog.heightBase       = f["heightBase"].get<float>();
        if (f.contains("noiseStrength"))    out.fog.noiseStrength    = f["noiseStrength"].get<float>();
        if (f.contains("noiseScale"))       out.fog.noiseScale       = f["noiseScale"].get<float>();
        if (f.contains("noiseSpeed"))       out.fog.noiseSpeed       = f["noiseSpeed"].get<float>();
        if (f.contains("nearClearDistance")) out.fog.nearClearDistance = f["nearClearDistance"].get<float>();
        if (f.contains("distanceDensity"))   out.fog.distanceDensity   = f["distanceDensity"].get<float>();
    }

    // Shadows
    if (root.contains("shadows"))
    {
        auto& s = root["shadows"];
        if (s.contains("distance")) out.shadows.distance = s["distance"].get<float>();
    }
    // Motion blur
    if (root.contains("motionBlur"))
    {
        auto& mb = root["motionBlur"];
        if (mb.contains("enabled"))       out.motionBlur.enabled       = mb["enabled"].get<bool>();
        if (mb.contains("shutterAngle"))  out.motionBlur.shutterAngle  = mb["shutterAngle"].get<float>();
        if (mb.contains("maxBlurPixels")) out.motionBlur.maxBlurPixels = mb["maxBlurPixels"].get<float>();
        if (mb.contains("samples"))       out.motionBlur.samples       = mb["samples"].get<int>();
    }
    // Reflection probes (validated by LevelAssetLoader)
    if (root.contains("reflectionProbes") && root["reflectionProbes"].is_array())
    {
        for (const auto& p : root["reflectionProbes"])
        {
            SceneDescriptor::ReflectionProbeDesc probe;
            for (int i = 0; i < 3; ++i)
            {
                probe.position[i] = p["position"][i].get<float>();
                probe.extents[i]  = p["extents"][i].get<float>();
            }
            if (p.contains("blendDistance")) probe.blendDistance = p["blendDistance"].get<float>();
            out.reflectionProbes.push_back(probe);
        }
    }

    return true;
}

bool SceneLoader::LoadParticlePreset(const std::string& path, SceneDescriptor::ParticleEmitterDesc& out)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        SE_LOG_ERROR("SceneLoader: Failed to open particle preset '%s'", path.c_str());
        return false;
    }

    json root;
    try
    {
        root = json::parse(file);
    }
    catch (const json::parse_error& e)
    {
        SE_LOG_ERROR("SceneLoader: JSON parse error in preset '%s': %s", path.c_str(), e.what());
        return false;
    }

    out = SceneDescriptor::ParticleEmitterDesc{};
    if (root.contains("emitter") && root["emitter"].is_object())
        ReadParticleEmitter(root["emitter"], out);
    else
        ReadParticleEmitter(root, out);
    return true;
}

std::vector<std::string> SceneLoader::ScanSceneDirectory(const std::string& directory)
{
    std::vector<std::string> results;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(directory, ec)) return results;

    for (auto& entry : fs::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            results.push_back(entry.path().string());
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::vector<std::string> SceneLoader::ScanParticlePresetDirectory(const std::string& directory)
{
    std::vector<std::string> results;
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(directory, ec)) return results;

    for (auto& entry : fs::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            results.push_back(entry.path().string());
    }

    std::sort(results.begin(), results.end());
    return results;
}

} // namespace SE
