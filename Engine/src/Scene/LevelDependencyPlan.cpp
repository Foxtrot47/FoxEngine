#include "Engine/Scene/LevelDependencyPlan.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace SE {

std::string LevelDependencyPlan::NormalizePath(const std::string& path)
{
    if (path.empty()) return {};
    std::filesystem::path p(path);
    p = p.lexically_normal();
    std::string result = p.generic_string();
    std::replace(result.begin(), result.end(), '\\', '/');
    while (result.size() > 2 && result.back() == '/') result.pop_back();
    return result;
}

LevelDependencyPlan LevelDependencyPlan::Build(const LevelAsset& level)
{
    LevelDependencyPlan plan;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& path, LevelDependencyKind kind,
                   bool required, const std::string& owner) {
        const std::string normalized = NormalizePath(path);
        if (normalized.empty()) return;
        const std::string key = std::to_string(static_cast<int>(kind)) + ":" + normalized;
        if (seen.insert(key).second)
            plan.dependencies.push_back({ normalized, owner, kind, required });
    };

    for (size_t i = 0; i < level.entities.size(); ++i)
    {
        const auto& e = level.entities[i];
        if (!e.active) continue;
        const std::string owner = "/entities/" + std::to_string(i);
        if (e.meshRenderer)
            add(e.meshRenderer->mesh, LevelDependencyKind::Mesh, true, owner + "/mesh");
        if (e.primitiveRenderer)
        {
            const auto& m = e.primitiveRenderer->material;
            add(m.albedo, LevelDependencyKind::Texture, false, owner + "/material/albedo");
            add(m.normal, LevelDependencyKind::Texture, false, owner + "/material/normal");
            add(m.roughness, LevelDependencyKind::Texture, false, owner + "/material/roughness");
            add(m.metallic, LevelDependencyKind::Texture, false, owner + "/material/metallic");
            add(m.emissive, LevelDependencyKind::Texture, false, owner + "/material/emissive");
        }
        if (e.particleEmitter)
            add(e.particleEmitter->texture, LevelDependencyKind::ParticleTexture, false,
                owner + "/particle/texture");
    }

    if (level.settings.contains("environment") && level.settings["environment"].is_object())
    {
        const auto& env = level.settings["environment"];
        for (const char* key : { "skybox", "reflectionPanorama" })
            if (env.contains(key) && env[key].is_string())
                add(env[key].get<std::string>(), LevelDependencyKind::Environment, false,
                    std::string("/settings/environment/") + key);
    }
    for (const auto& path : level.dependencies)
    {
        const std::string normalized = NormalizePath(path);
        const bool classified = std::any_of(plan.dependencies.begin(), plan.dependencies.end(),
            [&](const LevelDependency& dependency) { return dependency.path == normalized; });
        if (!classified)
            add(path, LevelDependencyKind::Texture, false, "/dependencies");
    }
    return plan;
}

const LevelDependency* LevelDependencyPlan::Find(const std::string& path,
                                                  LevelDependencyKind kind) const
{
    const std::string normalized = NormalizePath(path);
    for (const auto& dep : dependencies)
        if (dep.kind == kind && dep.path == normalized) return &dep;
    return nullptr;
}

} // namespace SE
