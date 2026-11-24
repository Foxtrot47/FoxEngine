#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string_view>

#include <nlohmann/json.hpp>

namespace SE {

using json = nlohmann::json;

namespace {

using Severity = LevelDiagnosticSeverity;

void AddDiagnostic(LevelAssetLoadResult& result, Severity severity,
                   std::string path, std::string message)
{
    result.diagnostics.push_back({ severity, std::move(path), std::move(message) });
}

bool IsFloatArray(const json& value, size_t count)
{
    if (!value.is_array() || value.size() != count)
        return false;
    return std::all_of(value.begin(), value.end(), [](const json& element) {
        return element.is_number();
    });
}

template<size_t N>
std::array<float, N> ReadFloatArray(const json& object, const char* field,
                                    std::array<float, N> fallback)
{
    if (!object.contains(field) || !IsFloatArray(object[field], N))
        return fallback;
    for (size_t i = 0; i < N; ++i)
        fallback[i] = object[field][i].get<float>();
    return fallback;
}

LevelParticleEmitterDefinition DecodeParticleEmitter(const json& value)
{
    LevelParticleEmitterDefinition out;
    out.position = ReadFloatArray(value, "position", out.position);
    out.velocityMin = ReadFloatArray(value, "velocity_min", out.velocityMin);
    out.velocityMax = ReadFloatArray(value, "velocity_max", out.velocityMax);
    out.colorStart = ReadFloatArray(value, "color_start", out.colorStart);
    out.colorEnd = ReadFloatArray(value, "color_end", out.colorEnd);
    out.gravity = ReadFloatArray(value, "gravity", out.gravity);
    out.spawnBoxExtents = ReadFloatArray(value, "spawnBoxExtents", out.spawnBoxExtents);

    if (value.contains("emit_rate")) out.emitRate = value["emit_rate"].get<float>();
    if (value.contains("max_particles")) out.maxParticles = value["max_particles"].get<int>();
    if (value.contains("lifetime_min")) out.lifetimeMin = value["lifetime_min"].get<float>();
    if (value.contains("lifetime_max")) out.lifetimeMax = value["lifetime_max"].get<float>();
    if (value.contains("size_start")) out.sizeStart = value["size_start"].get<float>();
    if (value.contains("size_end")) out.sizeEnd = value["size_end"].get<float>();
    if (value.contains("spawn_radius")) out.spawnRadius = value["spawn_radius"].get<float>();
    if (value.contains("spawnShape")) out.spawnShape = value["spawnShape"].get<std::string>() == "box" ? 1 : 0;
    if (value.contains("texture")) out.texture = value["texture"].get<std::string>();
    if (value.contains("atlasColumns")) out.atlasColumns = value["atlasColumns"].get<int>();
    if (value.contains("atlasRows")) out.atlasRows = value["atlasRows"].get<int>();
    if (value.contains("atlasFrameCount")) out.atlasFrameCount = value["atlasFrameCount"].get<int>();
    if (value.contains("atlasSpeed")) out.atlasSpeed = value["atlasSpeed"].get<float>();
    if (value.contains("textureRotationDegrees")) out.textureRotationDegrees = value["textureRotationDegrees"].get<float>();
    if (value.contains("softDistance")) out.softDistance = value["softDistance"].get<float>();
    if (value.contains("aspectRatio")) out.aspectRatio = value["aspectRatio"].get<float>();
    if (value.contains("blendMode")) out.blendMode = value["blendMode"].get<int>();
    if (value.contains("drag")) out.drag = value["drag"].get<float>();
    if (value.contains("shaderMode")) out.shaderMode = value["shaderMode"].get<int>();
    if (value.contains("emitMode")) out.emitMode = value["emitMode"].get<int>();
    if (value.contains("rotationDegrees")) out.rotationDegrees = value["rotationDegrees"].get<float>();
    if (value.contains("randomRotation")) out.randomRotation = value["randomRotation"].get<bool>();
    if (value.contains("tracerStretch")) out.tracerStretch = value["tracerStretch"].get<float>();
    if (value.contains("lightingInfluence")) out.lightingInfluence = value["lightingInfluence"].get<float>();
    return out;
}

void DecodeComponent(LevelEntityDefinition& entity, const std::string& type,
                     const json& value)
{
    if (type == "transform")
    {
        LevelTransformDefinition out;
        out.position = ReadFloatArray(value, "position", out.position);
        out.rotationDegrees = ReadFloatArray(value, "rotationDegrees", out.rotationDegrees);
        out.scale = ReadFloatArray(value, "scale", out.scale);
        entity.transform = std::move(out);
    }
    else if (type == "meshRenderer")
    {
        LevelMeshRendererDefinition out;
        if (value.contains("mesh")) out.mesh = value["mesh"].get<std::string>();
        if (value.contains("alphaCutoff")) out.alphaCutoff = value["alphaCutoff"].get<float>();
        if (value.contains("castShadows")) out.castShadows = value["castShadows"].get<bool>();
        if (value.contains("receiveShadows")) out.receiveShadows = value["receiveShadows"].get<bool>();
        if (value.contains("collision")) out.collision = value["collision"].get<bool>();
        if (value.contains("visible")) out.visible = value["visible"].get<bool>();
        entity.meshRenderer = std::move(out);
    }
    else if (type == "camera")
    {
        LevelCameraDefinition out;
        out.eye = ReadFloatArray(value, "eye", out.eye);
        if (value.contains("yaw")) out.yaw = value["yaw"].get<float>();
        if (value.contains("pitch")) out.pitch = value["pitch"].get<float>();
        if (value.contains("nearZ")) out.nearZ = value["nearZ"].get<float>();
        if (value.contains("farZ")) out.farZ = value["farZ"].get<float>();
        if (value.contains("primary")) out.primary = value["primary"].get<bool>();
        entity.camera = std::move(out);
    }
    else if (type == "directionalLight")
    {
        LevelDirectionalLightDefinition out;
        out.color = ReadFloatArray(value, "color", out.color);
        if (value.contains("elevation")) out.elevation = value["elevation"].get<float>();
        if (value.contains("azimuth")) out.azimuth = value["azimuth"].get<float>();
        if (value.contains("intensity")) out.intensity = value["intensity"].get<float>();
        entity.directionalLight = std::move(out);
    }
    else if (type == "pointLight")
    {
        LevelPointLightDefinition out;
        out.position = ReadFloatArray(value, "position", out.position);
        out.color = ReadFloatArray(value, "color", out.color);
        if (value.contains("radius")) out.radius = value["radius"].get<float>();
        if (value.contains("castShadow")) out.castShadow = value["castShadow"].get<bool>();
        if (value.contains("flicker")) out.flicker = value["flicker"].get<bool>();
        if (value.contains("flickerAmplitude")) out.flickerAmplitude = value["flickerAmplitude"].get<float>();
        if (value.contains("flickerFrequency")) out.flickerFrequency = value["flickerFrequency"].get<float>();
        if (value.contains("flickerSeed")) out.flickerSeed = value["flickerSeed"].get<uint32_t>();
        entity.pointLight = std::move(out);
    }
    else if (type == "particleEmitter")
        entity.particleEmitter = DecodeParticleEmitter(value);
    else if (type == "sphereRenderer" || type == "planeRenderer")
    {
        LevelPrimitiveRendererDefinition out;
        out.type = type == "planeRenderer" ? LevelPrimitiveType::Plane : LevelPrimitiveType::Sphere;
        out.position = ReadFloatArray(value, "position", out.position);
        if (value.contains("radius")) out.radius = value["radius"].get<float>();
        if (value.contains("halfSizeX")) out.halfSizeX = value["halfSizeX"].get<float>();
        if (value.contains("halfSizeZ")) out.halfSizeZ = value["halfSizeZ"].get<float>();
        if (value.contains("material") && value["material"].is_object())
        {
            const auto& material = value["material"];
            for (const auto& [field, target] : std::initializer_list<std::pair<const char*, std::string*>>{
                { "albedo", &out.material.albedo }, { "normal", &out.material.normal },
                { "roughness", &out.material.roughness }, { "metallic", &out.material.metallic },
                { "emissive", &out.material.emissive } })
                if (material.contains(field)) *target = material[field].get<std::string>();
            if (material.contains("emissive_intensity"))
                out.material.emissiveIntensity = material["emissive_intensity"].get<float>();
            out.material.emissiveColor = ReadFloatArray(
                material, "emissive_color", out.material.emissiveColor);
            out.material.tint = ReadFloatArray(material, "tint", out.material.tint);
            if (material.contains("reflectionMode"))
                out.material.planarReflection = material["reflectionMode"].get<std::string>() == "planar";
            if (material.contains("reflectionStrength"))
                out.material.reflectionStrength = material["reflectionStrength"].get<float>();
            if (material.contains("reflectionResolutionScale"))
                out.material.reflectionResolutionScale = material["reflectionResolutionScale"].get<float>();
            if (material.contains("roughnessScale"))
                out.material.roughnessScale = material["roughnessScale"].get<float>();
            if (material.contains("metallicScale"))
                out.material.metallicScale = material["metallicScale"].get<float>();
        }
        entity.primitiveRenderer = std::move(out);
    }
}

bool ParseEntityID(const json& value, LevelEntityID& out)
{
    if (!value.is_string())
        return false;

    const std::string text = value.get<std::string>();
    if (text.empty() || text.size() > 16 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        }))
        return false;

    try
    {
        out = std::stoull(text, nullptr, 16);
        return out != 0;
    }
    catch (...)
    {
        return false;
    }
}

bool NormalizeAssetPath(const json& value, std::string& normalized)
{
    if (!value.is_string())
        return false;

    std::string text = value.get<std::string>();
    std::replace(text.begin(), text.end(), '\\', '/');
    std::filesystem::path path(text);
    if (text.empty() || path.is_absolute())
        return false;

    path = path.lexically_normal();
    auto it = path.begin();
    if (it == path.end() || it->generic_string() != "Assets")
        return false;
    for (const auto& part : path)
        if (part == "..")
            return false;

    normalized = path.generic_string();
    return true;
}

void AddDependency(LevelAssetLoadResult& result, const json& value,
                   const std::string& diagnosticPath)
{
    std::string path;
    if (!NormalizeAssetPath(value, path))
    {
        AddDiagnostic(result, Severity::Error, diagnosticPath,
                      "expected a project-relative path below Assets/");
        return;
    }
    result.asset.dependencies.push_back(std::move(path));
}

void ValidateTransform(LevelAssetLoadResult& result, const json& component,
                       const std::string& path)
{
    for (const char* field : { "position", "rotationDegrees", "scale" })
    {
        if (component.contains(field) && !IsFloatArray(component[field], 3))
            AddDiagnostic(result, Severity::Error, path + "/" + field,
                          "expected an array of three numbers");
    }
    for (auto it = component.begin(); it != component.end(); ++it)
    {
        const std::string& key = it.key();
        if (key != "position" && key != "rotationDegrees" && key != "scale")
            AddDiagnostic(result, Severity::Warning, path + "/" + key,
                          key == "rotation" ? "unknown field is ignored (use rotationDegrees)"
                                            : "unknown field is ignored");
    }
}

void ValidateNumber(LevelAssetLoadResult& result, const json& component,
                    const char* field, const std::string& path)
{
    if (component.contains(field) && !component[field].is_number())
        AddDiagnostic(result, Severity::Error, path + "/" + field, "expected a number");
}

void ValidateInteger(LevelAssetLoadResult& result, const json& component,
                     const char* field, const std::string& path)
{
    if (component.contains(field) && !component[field].is_number_integer())
        AddDiagnostic(result, Severity::Error, path + "/" + field, "expected an integer");
}

void ValidateBoolean(LevelAssetLoadResult& result, const json& component,
                     const char* field, const std::string& path)
{
    if (component.contains(field) && !component[field].is_boolean())
        AddDiagnostic(result, Severity::Error, path + "/" + field, "expected a boolean");
}

void ValidateVector(LevelAssetLoadResult& result, const json& component,
                    const char* field, size_t count, const std::string& path)
{
    if (component.contains(field) && !IsFloatArray(component[field], count))
        AddDiagnostic(result, Severity::Error, path + "/" + field,
                      "expected an array of " + std::to_string(count) + " numbers");
}

void CollectMaterialDependencies(LevelAssetLoadResult& result, const json& component,
                                 const std::string& path)
{
    if (!component.contains("material"))
        return;
    const auto& material = component["material"];
    if (!material.is_object())
    {
        AddDiagnostic(result, Severity::Error, path + "/material", "expected an object");
        return;
    }

    for (const char* field : { "albedo", "normal", "roughness", "metallic", "emissive" })
        if (material.contains(field))
            AddDependency(result, material[field], path + "/material/" + field);
}

void ValidateComponent(LevelAssetLoadResult& result, const std::string& type,
                       const json& component, const std::string& path)
{
    static const std::unordered_set<std::string> kKnownComponents = {
        "transform", "meshRenderer", "camera", "directionalLight", "pointLight",
        "particleEmitter", "sphereRenderer", "planeRenderer"
    };

    if (kKnownComponents.find(type) == kKnownComponents.end())
    {
        AddDiagnostic(result, Severity::Error, path, "unknown component type '" + type + "'");
        return;
    }
    if (!component.is_object())
    {
        AddDiagnostic(result, Severity::Error, path, "expected an object");
        return;
    }

    if (type == "transform")
        ValidateTransform(result, component, path);
    else if (type == "meshRenderer")
    {
        if (!component.contains("mesh"))
            AddDiagnostic(result, Severity::Error, path + "/mesh", "required field is missing");
        else
            AddDependency(result, component["mesh"], path + "/mesh");
        ValidateNumber(result, component, "alphaCutoff", path);
        ValidateBoolean(result, component, "castShadows", path);
        ValidateBoolean(result, component, "receiveShadows", path);
        ValidateBoolean(result, component, "collision", path);
        ValidateBoolean(result, component, "visible", path);
    }
    else if (type == "camera")
    {
        ValidateVector(result, component, "eye", 3, path);
        for (const char* field : { "yaw", "pitch", "nearZ", "farZ" })
            ValidateNumber(result, component, field, path);
        ValidateBoolean(result, component, "primary", path);
        if (component.contains("nearZ") && component["nearZ"].is_number() &&
            component["nearZ"].get<float>() <= 0.0f)
            AddDiagnostic(result, Severity::Error, path + "/nearZ", "must be greater than zero");
        if (component.contains("farZ") && component["farZ"].is_number() &&
            component["farZ"].get<float>() <= 0.0f)
            AddDiagnostic(result, Severity::Error, path + "/farZ", "must be greater than zero");
    }
    else if (type == "directionalLight")
    {
        ValidateVector(result, component, "color", 3, path);
        for (const char* field : { "elevation", "azimuth", "intensity" })
            ValidateNumber(result, component, field, path);
    }
    else if (type == "pointLight")
    {
        ValidateVector(result, component, "position", 3, path);
        ValidateVector(result, component, "color", 3, path);
        ValidateNumber(result, component, "radius", path);
        ValidateBoolean(result, component, "castShadow", path);
        ValidateBoolean(result, component, "flicker", path);
        ValidateNumber(result, component, "flickerAmplitude", path);
        ValidateNumber(result, component, "flickerFrequency", path);
        ValidateInteger(result, component, "flickerSeed", path);
        if (component.contains("radius") && component["radius"].is_number() &&
            component["radius"].get<float>() <= 0.0f)
            AddDiagnostic(result, Severity::Error, path + "/radius", "must be greater than zero");
    }
    else if (type == "particleEmitter")
    {
        if (component.contains("preset"))
            AddDependency(result, component["preset"], path + "/preset");
        if (component.contains("texture"))
            AddDependency(result, component["texture"], path + "/texture");
        for (const char* field : { "position", "velocity_min", "velocity_max", "gravity", "spawnBoxExtents" })
            ValidateVector(result, component, field, 3, path);
        for (const char* field : { "color_start", "color_end" })
            ValidateVector(result, component, field, 4, path);
        for (const char* field : { "emit_rate", "lifetime_min", "lifetime_max", "size_start",
                                  "size_end", "spawn_radius", "atlasSpeed", "softDistance",
                                  "aspectRatio", "drag", "rotationDegrees", "tracerStretch",
                                  "lightingInfluence" })
            ValidateNumber(result, component, field, path);
        for (const char* field : { "max_particles", "atlasColumns", "atlasRows",
                                  "atlasFrameCount", "blendMode", "shaderMode", "emitMode" })
            ValidateInteger(result, component, field, path);
        ValidateBoolean(result, component, "randomRotation", path);
        if (component.contains("spawnShape") &&
            (!component["spawnShape"].is_string() ||
             (component["spawnShape"] != "sphere" && component["spawnShape"] != "box")))
            AddDiagnostic(result, Severity::Error, path + "/spawnShape",
                          "expected 'sphere' or 'box'");
    }
    else if (type == "sphereRenderer" || type == "planeRenderer")
    {
        CollectMaterialDependencies(result, component, path);
        ValidateVector(result, component, "position", 3, path);
        for (const char* field : { "radius", "halfSizeX", "halfSizeZ" })
            ValidateNumber(result, component, field, path);
        if (component.contains("material") && component["material"].is_object())
        {
            const auto& material = component["material"];
            ValidateNumber(result, material, "reflectionStrength", path + "/material");
            ValidateNumber(result, material, "reflectionResolutionScale", path + "/material");
            ValidateNumber(result, material, "roughnessScale", path + "/material");
            ValidateNumber(result, material, "metallicScale", path + "/material");
            if (material.contains("reflectionMode") &&
                (!material["reflectionMode"].is_string() ||
                 (material["reflectionMode"] != "environment" && material["reflectionMode"] != "planar")))
                AddDiagnostic(result, Severity::Error, path + "/material/reflectionMode",
                              "expected 'environment' or 'planar'");
        }
    }
}

void ValidateHierarchy(LevelAssetLoadResult& result)
{
    std::unordered_map<LevelEntityID, size_t> byID;
    for (size_t i = 0; i < result.asset.entities.size(); ++i)
        byID[result.asset.entities[i].id] = i;

    for (size_t i = 0; i < result.asset.entities.size(); ++i)
    {
        const auto& entity = result.asset.entities[i];
        if (entity.parent != 0 && byID.find(entity.parent) == byID.end())
            AddDiagnostic(result, Severity::Error, "/entities/" + std::to_string(i) + "/parent",
                          "parent entity does not exist");
    }

    enum class Visit : uint8_t { None, Active, Complete };
    std::unordered_map<LevelEntityID, Visit> visits;
    std::function<void(LevelEntityID)> visit = [&](LevelEntityID id) {
        Visit& state = visits[id];
        if (state == Visit::Complete)
            return;
        if (state == Visit::Active)
        {
            AddDiagnostic(result, Severity::Error, "/entities", "entity parent cycle detected");
            return;
        }
        state = Visit::Active;
        const auto found = byID.find(id);
        if (found != byID.end())
        {
            const LevelEntityID parent = result.asset.entities[found->second].parent;
            if (parent != 0 && byID.find(parent) != byID.end())
                visit(parent);
        }
        state = Visit::Complete;
    };

    for (const auto& entity : result.asset.entities)
        visit(entity.id);
}

void ParseLevelHeader(LevelAssetLoadResult& result, const json& root)
{
    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != "FoxLevel")
        AddDiagnostic(result, Severity::Error, "/format", "expected 'FoxLevel'");

    if (!root.contains("version") ||
        (!root["version"].is_number_unsigned() && !root["version"].is_number_integer()))
        AddDiagnostic(result, Severity::Error, "/version", "expected an unsigned format version");
    else
    {
        const int64_t version = root["version"].get<int64_t>();
        if (version < 0 || version > UINT32_MAX)
            AddDiagnostic(result, Severity::Error, "/version", "format version is out of range");
        else
            result.asset.version = static_cast<uint32_t>(version);
        if (result.asset.version != LevelAsset::kCurrentVersion)
            AddDiagnostic(result, Severity::Error, "/version", "unsupported level format version");
    }

    if (root.contains("name") && root["name"].is_string())
        result.asset.name = root["name"].get<std::string>();
    else
        AddDiagnostic(result, Severity::Error, "/name", "expected a string");
}

void ParseEnvironmentSettings(LevelAssetLoadResult& result, const json& root)
{
    if (root.contains("settings"))
    {
        if (root["settings"].is_object())
            result.asset.settings = root["settings"];
        else
            AddDiagnostic(result, Severity::Error, "/settings", "expected an object");
    }

    if (result.asset.settings.contains("environment"))
    {
        const auto& environment = result.asset.settings["environment"];
        if (!environment.is_object())
            AddDiagnostic(result, Severity::Error, "/settings/environment", "expected an object");
        else
        {
            if (environment.contains("skybox"))
                AddDependency(result, environment["skybox"], "/settings/environment/skybox");
            if (environment.contains("reflectionPanorama"))
                AddDependency(result, environment["reflectionPanorama"],
                              "/settings/environment/reflectionPanorama");
            ValidateNumber(result, environment, "iblIntensity", "/settings/environment");
            ValidateNumber(result, environment, "diffuseIBLSaturation", "/settings/environment");
            if (environment.contains("diffuseIBLSaturation") &&
                environment["diffuseIBLSaturation"].is_number())
            {
                const float saturation = environment["diffuseIBLSaturation"].get<float>();
                if (saturation < 0.0f || saturation > 1.0f)
                    AddDiagnostic(result, Severity::Error,
                                  "/settings/environment/diffuseIBLSaturation",
                                  "must be between zero and one");
            }

        }
    }
}

// Validates a project-relative .lua path, normalizing separators.
bool ParseLuaPath(LevelAssetLoadResult& result, const json& value, const std::string& path,
                  std::string& out)
{
    if (!value.is_string())
    {
        AddDiagnostic(result, Severity::Error, path, "expected a string");
        return false;
    }
    out = value.get<std::string>();
    std::replace(out.begin(), out.end(), '\\', '/');
    constexpr std::string_view k_luaExtension = ".lua";
    if (out.size() <= k_luaExtension.size() ||
        out.compare(out.size() - k_luaExtension.size(), k_luaExtension.size(), k_luaExtension) != 0)
    {
        AddDiagnostic(result, Severity::Error, path, "expected a path to a .lua file");
        return false;
    }
    return true;
}

// Script and actor class paths are not asset dependencies: the script system
// resolves and loads them, so only their shape is validated here.
void ParseEntityScripts(LevelAssetLoadResult& result, const json& scripts,
                        const std::string& path, std::vector<LevelScriptDefinition>& out)
{
    if (!scripts.is_array())
    {
        AddDiagnostic(result, Severity::Error, path, "expected an array");
        return;
    }

    for (size_t i = 0; i < scripts.size(); ++i)
    {
        const auto& item = scripts[i];
        const std::string itemPath = path + "/" + std::to_string(i);
        if (!item.is_object())
        {
            AddDiagnostic(result, Severity::Error, itemPath, "expected an object");
            continue;
        }

        LevelScriptDefinition script;
        bool valid = true;
        if (!item.contains("script"))
        {
            AddDiagnostic(result, Severity::Error, itemPath + "/script", "required field is missing");
            valid = false;
        }
        else if (!ParseLuaPath(result, item["script"], itemPath + "/script", script.script))
        {
            valid = false;
        }

        if (item.contains("properties"))
        {
            if (item["properties"].is_object())
                script.properties = item["properties"];
            else
            {
                AddDiagnostic(result, Severity::Error, itemPath + "/properties", "expected an object");
                valid = false;
            }
        }

        for (auto it = item.begin(); it != item.end(); ++it)
            if (it.key() != "script" && it.key() != "properties")
                AddDiagnostic(result, Severity::Warning, itemPath + "/" + it.key(),
                              "unknown field is ignored");

        if (valid)
            out.push_back(std::move(script));
    }
}

void ParseEntityDefinition(LevelAssetLoadResult& result, const json& source,
                           const std::string& base, std::unordered_set<LevelEntityID>& ids)
{
    LevelEntityDefinition entity;
    if (!source.contains("id") || !ParseEntityID(source["id"], entity.id))
        AddDiagnostic(result, Severity::Error, base + "/id",
                      "expected a non-zero hexadecimal string of at most 16 digits");
    else if (!ids.insert(entity.id).second)
        AddDiagnostic(result, Severity::Error, base + "/id", "duplicate entity ID");

    if (source.contains("parent") && !ParseEntityID(source["parent"], entity.parent))
        AddDiagnostic(result, Severity::Error, base + "/parent",
                      "expected a non-zero hexadecimal entity ID");
    if (source.contains("name"))
    {
        if (source["name"].is_string())
            entity.name = source["name"].get<std::string>();
        else
            AddDiagnostic(result, Severity::Error, base + "/name", "expected a string");
    }
    if (source.contains("active"))
    {
        if (source["active"].is_boolean())
            entity.active = source["active"].get<bool>();
        else
            AddDiagnostic(result, Severity::Error, base + "/active", "expected a boolean");
    }

    // An actor entity's components are its root transform plus override
    // objects for the class's components; only the transform is validated
    // here, the class schema is checked when the actor is resolved.
    if (source.contains("actor"))
    {
        LevelActorDefinition actor;
        if (ParseLuaPath(result, source["actor"], base + "/actor", actor.actorClass))
            entity.actor = std::move(actor);
    }

    if (!source.contains("components") || !source["components"].is_object())
        AddDiagnostic(result, Severity::Error, base + "/components", "expected an object");
    else
    {
        for (auto it = source["components"].begin(); it != source["components"].end(); ++it)
        {
            const std::string componentPath = base + "/components/" + it.key();
            if (source.contains("actor") && it.key() != "transform")
            {
                if (!it.value().is_object())
                    AddDiagnostic(result, Severity::Error, componentPath,
                                  "expected an object of overrides for the actor component '" + it.key() + "'");
                else if (entity.actor)
                    entity.actor->components[it.key()] = it.value();
                continue;
            }
            const size_t diagnosticsBefore = result.diagnostics.size();
            ValidateComponent(result, it.key(), it.value(), componentPath);
            if (it.value().is_object() && result.diagnostics.size() == diagnosticsBefore)
                DecodeComponent(entity, it.key(), it.value());
        }
    }

    constexpr const char* k_behaviourHint =
        "only actors have behaviour: place an actor (\"actor\": \"Assets/Scripts/....lua\") instead";
    if (source.contains("properties"))
    {
        if (!source.contains("actor"))
            AddDiagnostic(result, Severity::Error, base + "/properties", k_behaviourHint);
        else if (!source["properties"].is_object())
            AddDiagnostic(result, Severity::Error, base + "/properties", "expected an object");
        else if (entity.actor)
            entity.actor->properties = source["properties"];
    }
    if (source.contains("scripts"))
    {
        if (source.contains("actor"))
            ParseEntityScripts(result, source["scripts"], base + "/scripts", entity.scripts);
        else
            AddDiagnostic(result, Severity::Error, base + "/scripts", k_behaviourHint);
    }
    result.asset.entities.push_back(std::move(entity));
}

void ParseVersion2(LevelAssetLoadResult& result, const json& root)
{
    result.asset = LevelAsset{};

    ParseLevelHeader(result, root);
    ParseEnvironmentSettings(result, root);

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        AddDiagnostic(result, Severity::Error, "/entities", "expected an array");
        return;
    }

    std::unordered_set<LevelEntityID> ids;
    for (size_t i = 0; i < root["entities"].size(); ++i)
    {
        const auto& source = root["entities"][i];
        const std::string base = "/entities/" + std::to_string(i);
        if (!source.is_object())
        {
            AddDiagnostic(result, Severity::Error, base, "expected an object");
            continue;
        }
        ParseEntityDefinition(result, source, base, ids);
    }

    ValidateHierarchy(result);
    std::sort(result.asset.dependencies.begin(), result.asset.dependencies.end());
    result.asset.dependencies.erase(
        std::unique(result.asset.dependencies.begin(), result.asset.dependencies.end()),
        result.asset.dependencies.end());
}

void ConvertLegacy(LevelAssetLoadResult& result, const json& root)
{
    json settings = json::object();
    json environment = json::object();
    for (const char* field : { "skybox", "reflectionPanorama", "iblIntensity", "diffuseIBLSaturation" })
        if (root.contains(field))
            environment[field] = root[field];
    if (!environment.empty())
        settings["environment"] = std::move(environment);
    for (const char* field : { "toneMapping", "colorGrading", "bloom", "fog", "physics" })
        if (root.contains(field))
            settings[field] = root[field];

    json canonical = {
        { "format", "FoxLevel" },
        { "version", LevelAsset::kCurrentVersion },
        { "name", root.value("name", "Untitled") },
        { "settings", std::move(settings) },
        { "entities", json::array() }
    };

    LevelEntityID nextID = 1;
    auto addEntity = [&](const std::string& name, json components) {
        char id[17];
        sprintf_s(id, "%016llx", static_cast<unsigned long long>(nextID++));
        canonical["entities"].push_back({
            { "id", id }, { "name", name }, { "active", true },
            { "components", std::move(components) }
        });
    };

    if (root.contains("mesh") && root["mesh"].is_object())
    {
        const auto& mesh = root["mesh"];
        json transform = mesh.value("transform", json::object());
        if (transform.contains("rotation"))
        {
            transform["rotationDegrees"] = transform["rotation"];
            transform.erase("rotation");
        }
        if (transform.contains("scale") && transform["scale"].is_number())
        {
            const float scale = transform["scale"].get<float>();
            transform["scale"] = { scale, scale, scale };
        }
        json renderer = json::object();
        if (mesh.contains("path")) renderer["mesh"] = mesh["path"];
        if (mesh.contains("alphaCutoff")) renderer["alphaCutoff"] = mesh["alphaCutoff"];
        addEntity("SceneMesh", {
            { "transform", std::move(transform) },
            { "meshRenderer", std::move(renderer) }
        });
    }

    if (root.contains("camera"))
        addEntity("Camera", { { "camera", root["camera"] } });
    if (root.contains("sun"))
        addEntity("Sun", { { "directionalLight", root["sun"] } });
    if (root.contains("pointLights") && root["pointLights"].is_array())
    {
        for (size_t i = 0; i < root["pointLights"].size(); ++i)
            addEntity("Point Light " + std::to_string(i + 1),
                      { { "pointLight", root["pointLights"][i] } });
    }
    if (root.contains("objects") && root["objects"].is_array())
    {
        for (size_t i = 0; i < root["objects"].size(); ++i)
        {
            const auto& object = root["objects"][i];
            const std::string type = object.value("type", "sphere");
            json convertedObject = object;
            if (convertedObject.contains("material") &&
                convertedObject["material"].contains("metallic_map"))
            {
                convertedObject["material"]["metallic"] =
                    convertedObject["material"]["metallic_map"];
                convertedObject["material"].erase("metallic_map");
            }
            json components = json::object();
            components[type == "plane" ? "planeRenderer" : "sphereRenderer"] =
                std::move(convertedObject);
            addEntity("Object " + std::to_string(i + 1), std::move(components));
        }
    }
    if (root.contains("particles") && root["particles"].is_array())
    {
        for (size_t i = 0; i < root["particles"].size(); ++i)
            addEntity("Particle Emitter " + std::to_string(i + 1),
                      { { "particleEmitter", root["particles"][i] } });
    }

    ParseVersion2(result, canonical);
    result.asset.convertedFromLegacy = true;
    result.diagnostics.insert(result.diagnostics.begin(), {
        Severity::Warning, "/",
        "legacy unversioned scene converted in memory; save it in the editor or run 'FoxProject upgrade'"
    });
}

} // namespace

LevelAssetLoadResult LevelAssetLoader::LoadFromFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        LevelAssetLoadResult result;
        AddDiagnostic(result, Severity::Error, "/", "failed to open '" + path + "'");
        return result;
    }

    try
    {
        return Parse(json::parse(file), path);
    }
    catch (const json::exception& error)
    {
        LevelAssetLoadResult result;
        AddDiagnostic(result, Severity::Error, "/",
                      "JSON parse error in '" + path + "': " + error.what());
        return result;
    }
}

LevelAssetLoadResult LevelAssetLoader::Parse(const json& root, const std::string& sourceName)
{
    LevelAssetLoadResult result;
    try
    {
        if (!root.is_object())
        {
            AddDiagnostic(result, Severity::Error, "/", "level root must be an object");
            return result;
        }

        if (root.contains("format") || root.contains("version"))
            ParseVersion2(result, root);
        else
            ConvertLegacy(result, root);
    }
    catch (const json::exception& error)
    {
        result = LevelAssetLoadResult{};
        AddDiagnostic(result, Severity::Error, "/",
                      "invalid level data in '" + sourceName + "': " + error.what());
    }
    catch (const std::exception& error)
    {
        result = LevelAssetLoadResult{};
        AddDiagnostic(result, Severity::Error, "/",
                      "failed to process '" + sourceName + "': " + error.what());
    }

    if (!result.Succeeded())
        SE_LOG_WARN("LevelAssetLoader: validation failed for '%s'", sourceName.c_str());
    return result;
}

} // namespace SE
