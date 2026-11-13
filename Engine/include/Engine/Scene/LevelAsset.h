#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Engine/Scene/LevelTypes.h"

namespace SE {

using LevelEntityID = uint64_t;

enum class LevelDiagnosticSeverity
{
    Warning,
    Error
};

struct LevelDiagnostic
{
    LevelDiagnosticSeverity severity = LevelDiagnosticSeverity::Error;
    std::string              path;
    std::string              message;
};

struct LevelEntityDefinition
{
    LevelEntityID id       = 0;
    LevelEntityID parent   = 0;
    std::string   name     = "Entity";
    bool          active   = true;

    std::optional<LevelTransformDefinition> transform;
    std::optional<LevelMeshRendererDefinition> meshRenderer;
    std::optional<LevelCameraDefinition> camera;
    std::optional<LevelDirectionalLightDefinition> directionalLight;
    std::optional<LevelPointLightDefinition> pointLight;
    std::optional<LevelPrimitiveRendererDefinition> primitiveRenderer;
    std::optional<LevelParticleEmitterDefinition> particleEmitter;

    // Entity-level "scripts" array (sibling of "components").
    std::vector<LevelScriptDefinition> scripts;
};

struct LevelAsset
{
    static constexpr uint32_t kCurrentVersion = 2;

    std::string format = "FoxLevel";
    uint32_t    version = kCurrentVersion;
    std::string name = "Untitled";
    nlohmann::json settings = nlohmann::json::object();
    std::vector<LevelEntityDefinition> entities;
    std::vector<std::string> dependencies;
    bool convertedFromLegacy = false;
};

struct LevelAssetLoadResult
{
    LevelAsset asset;
    std::vector<LevelDiagnostic> diagnostics;

    bool Succeeded() const
    {
        for (const auto& diagnostic : diagnostics)
            if (diagnostic.severity == LevelDiagnosticSeverity::Error)
                return false;
        return true;
    }
};

} // namespace SE
