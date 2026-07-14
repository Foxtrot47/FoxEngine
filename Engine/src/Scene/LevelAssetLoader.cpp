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
        "spotLight", "particleEmitter", "rigidBody", "sphereRenderer", "planeRenderer"
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
    }
    else if (type == "particleEmitter")
    {
        if (component.contains("preset"))
            AddDependency(result, component["preset"], path + "/preset");
        if (component.contains("texture"))
            AddDependency(result, component["texture"], path + "/texture");
    }
    else if (type == "sphereRenderer" || type == "planeRenderer")
        CollectMaterialDependencies(result, component, path);
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

void ParseVersion2(LevelAssetLoadResult& result, const json& root)
{
    result.asset = LevelAsset{};

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
        }
    }

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

        if (!source.contains("components") || !source["components"].is_object())
            AddDiagnostic(result, Severity::Error, base + "/components", "expected an object");
        else
        {
            for (auto it = source["components"].begin(); it != source["components"].end(); ++it)
            {
                entity.components.emplace(it.key(), it.value());
                ValidateComponent(result, it.key(), it.value(), base + "/components/" + it.key());
            }
        }
        result.asset.entities.push_back(std::move(entity));
    }

    ValidateHierarchy(result);
    std::sort(result.asset.dependencies.begin(), result.asset.dependencies.end());
    result.asset.dependencies.erase(
        std::unique(result.asset.dependencies.begin(), result.asset.dependencies.end()),
        result.asset.dependencies.end());
}

LevelEntityDefinition& AddLegacyEntity(LevelAsset& asset, LevelEntityID id,
                                       const std::string& name)
{
    asset.entities.push_back({});
    auto& entity = asset.entities.back();
    entity.id = id;
    entity.name = name;
    return entity;
}

void ConvertLegacy(LevelAssetLoadResult& result, const json& root)
{
    result.asset = LevelAsset{};
    result.asset.convertedFromLegacy = true;
    if (root.contains("name") && root["name"].is_string())
        result.asset.name = root["name"].get<std::string>();

    AddDiagnostic(result, Severity::Warning, "/",
                  "legacy unversioned scene converted in memory; save it as FoxLevel version 2");

    json environment = json::object();
    for (const char* field : { "skybox", "reflectionPanorama", "iblIntensity" })
        if (root.contains(field))
            environment[field] = root[field];
    if (!environment.empty())
        result.asset.settings["environment"] = std::move(environment);
    for (const char* field : { "toneMapping", "bloom", "fog", "physics" })
        if (root.contains(field))
            result.asset.settings[field] = root[field];

    LevelEntityID nextID = 1;
    if (root.contains("mesh") && root["mesh"].is_object())
    {
        auto& entity = AddLegacyEntity(result.asset, nextID++, "SceneMesh");
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
        entity.components["transform"] = std::move(transform);
        json renderer = json::object();
        if (mesh.contains("path")) renderer["mesh"] = mesh["path"];
        if (mesh.contains("alphaCutoff")) renderer["alphaCutoff"] = mesh["alphaCutoff"];
        entity.components["meshRenderer"] = std::move(renderer);
    }

    if (root.contains("camera"))
    {
        auto& entity = AddLegacyEntity(result.asset, nextID++, "Camera");
        entity.components["camera"] = root["camera"];
    }
    if (root.contains("sun"))
    {
        auto& entity = AddLegacyEntity(result.asset, nextID++, "Sun");
        entity.components["directionalLight"] = root["sun"];
    }
    if (root.contains("pointLights") && root["pointLights"].is_array())
    {
        for (size_t i = 0; i < root["pointLights"].size(); ++i)
        {
            auto& entity = AddLegacyEntity(result.asset, nextID++,
                                           "Point Light " + std::to_string(i + 1));
            entity.components["pointLight"] = root["pointLights"][i];
        }
    }
    if (root.contains("objects") && root["objects"].is_array())
    {
        for (size_t i = 0; i < root["objects"].size(); ++i)
        {
            const auto& object = root["objects"][i];
            auto& entity = AddLegacyEntity(result.asset, nextID++,
                                           "Object " + std::to_string(i + 1));
            const std::string type = object.value("type", "sphere");
            json convertedObject = object;
            if (convertedObject.contains("material") &&
                convertedObject["material"].contains("metallic_map"))
            {
                convertedObject["material"]["metallic"] =
                    convertedObject["material"]["metallic_map"];
                convertedObject["material"].erase("metallic_map");
            }
            entity.components[type == "plane" ? "planeRenderer" : "sphereRenderer"] =
                std::move(convertedObject);
        }
    }
    if (root.contains("particles") && root["particles"].is_array())
    {
        for (size_t i = 0; i < root["particles"].size(); ++i)
        {
            auto& entity = AddLegacyEntity(result.asset, nextID++,
                                           "Particle Emitter " + std::to_string(i + 1));
            entity.components["particleEmitter"] = root["particles"][i];
        }
    }

    // Run the canonical validator over the converted representation so legacy files
    // receive the same path and dependency checks as native v2 levels.
    json canonical = {
        { "format", result.asset.format },
        { "version", result.asset.version },
        { "name", result.asset.name },
        { "settings", result.asset.settings },
        { "entities", json::array() }
    };
    for (const auto& entity : result.asset.entities)
    {
        json components = json::object();
        for (const auto& [type, component] : entity.components)
            components[type] = component;
        canonical["entities"].push_back({
            { "id", [&]() {
                char id[17];
                sprintf_s(id, "%016llx", static_cast<unsigned long long>(entity.id));
                return std::string(id);
            }() },
            { "name", entity.name },
            { "active", entity.active },
            { "components", std::move(components) }
        });
    }

    const auto warning = result.diagnostics.front();
    LevelAssetLoadResult validated;
    ParseVersion2(validated, canonical);
    validated.asset.convertedFromLegacy = true;
    validated.diagnostics.insert(validated.diagnostics.begin(), warning);
    result = std::move(validated);
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
