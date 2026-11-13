#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "Engine/Scene/LevelTypes.h"

namespace SE {

enum class ActorComponentType { Mesh, PointLight, ParticleEmitter, Box, Sphere };
enum class ActorBodyType { Kinematic, Dynamic };

// One component of an actor class (fox.actor { components = { ... } }) with a
// level instance's overrides applied. The transform is relative to the actor.
// Keys use the Lua spelling (snake_case) in both the class table and the
// level's per-instance override objects.
struct ActorComponentDefinition
{
    std::string name;
    ActorComponentType type = ActorComponentType::Mesh;
    LevelTransformDefinition transform;

    // mesh
    std::string mesh;
    float alphaCutoff = 0.0f;
    bool castShadows = true;
    bool receiveShadows = true;
    bool collision = false;      // kinematic triangle collider that follows the mesh

    // point_light (never casts shadows: point shadow slots belong to level lights);
    // box/sphere: albedo of the rendered shape (cast_shadows also applies)
    std::array<float, 3> color = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;

    // point_light range and sphere collider radius
    float radius = 0.5f;

    // particle_emitter: the preset file and the definition loaded from it
    // (ResolveActorComponents fills emitter; its position is ignored).
    std::string preset;
    LevelParticleEmitterDefinition emitter;

    // box and sphere: rendered shape and collider
    std::array<float, 3> size = { 1.0f, 1.0f, 1.0f }; // box full extents
    bool visible = true; // false: collider only (triggers, invisible walls)
    ActorBodyType body = ActorBodyType::Kinematic;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.2f;

    bool IsCollider() const { return type == ActorComponentType::Box || type == ActorComponentType::Sphere; }
};

const char* ActorComponentTypeName(ActorComponentType type);

// Validates and decodes one merged component table. Unknown keys and wrong
// value types are errors (the message names the key), so typos surface.
bool ParseActorComponent(const nlohmann::json& source, ActorComponentDefinition& out,
                         std::string& error);

struct ActorResolveResult
{
    std::vector<ActorComponentDefinition> components;
    std::vector<std::string> warnings;   // ignored overrides, missing presets
    std::string error;                   // the class itself is invalid
    bool Succeeded() const { return error.empty(); }
};

// An actor class as the script system reports it: its component array (base
// classes merged) or why the class could not be loaded.
struct ResolvedActorClass
{
    nlohmann::json components = nlohmann::json::array();
    std::string    error;
};
using ActorClassMap = std::unordered_map<std::string, ResolvedActorClass>; // by class path

// classComponents: the class's component array (ScriptDescription::components,
// base classes merged). overrides: a placement's objects keyed by component
// name. Invalid overrides are dropped with a warning, keeping the class value;
// particle presets are loaded from disk.
ActorResolveResult ResolveActorComponents(const nlohmann::json& classComponents,
                                          const nlohmann::json& overrides);

} // namespace SE
