#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <DirectXMath.h>
#include "Engine/Physics/Ray.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"
#include "Scripting/ScriptTypes.h"

using namespace DirectX;

namespace SE::Scripting {

namespace {

constexpr double k_pi = 3.14159265358979323846;
constexpr double k_radToDeg = 180.0 / k_pi;
constexpr double k_degToRad = k_pi / 180.0;
// Keeps look directions away from the up axis, where LookAt degenerates.
constexpr double k_maxPitchDeg = 89.9;

// --- Resolution ---------------------------------------------------------------

SceneRuntime& RequireRuntime(ScriptContext& ctx, const char* api)
{
    if (!ctx.host.scene)
        Raise("%s is not available in this context (no scene runtime)", api);
    return *ctx.host.scene;
}

SceneRuntime& RequireScene(ScriptContext& ctx, const char* api)
{
    SceneRuntime& scene = RequireRuntime(ctx, api);
    if (!scene.HasScene())
        Raise("%s: no scene is loaded", api);
    return scene;
}

Entity& ResolveEntity(ScriptContext& ctx, const ScriptEntity& handle)
{
    SceneRuntime& scene = RequireScene(ctx, "fox.Entity");
    if (handle.generation != ctx.sceneGeneration)
        Raise("entity %u belongs to a scene that is no longer loaded", handle.id);
    Entity* entity = scene.world.GetScene().FindEntity(handle.id);
    if (!entity)
        Raise("entity %u no longer exists", handle.id);
    return *entity;
}

TransformComponent& ResolveTransform(ScriptContext& ctx, const ScriptEntity& handle)
{
    Entity& entity = ResolveEntity(ctx, handle);
    TransformComponent* transform = entity.GetComponent<TransformComponent>();
    if (!transform)
        Raise("entity '%s' has no transform", entity.GetName().c_str());
    return *transform;
}

bool IsLive(const ScriptContext& ctx, const ScriptEntity& handle)
{
    const SceneRuntime* scene = ctx.host.scene;
    return scene && scene->HasScene() && handle.generation == ctx.sceneGeneration &&
           scene->world.GetScene().FindEntity(handle.id) != nullptr;
}

sol::object EntityOrNil(sol::state_view lua, const ScriptContext& ctx, const Entity* entity)
{
    if (!entity)
        return sol::lua_nil;
    return sol::make_object(lua, MakeEntityHandle(ctx, entity->GetID()));
}

// Lights, emitters and primitives keep their own positions; push the moved
// transform (and its children) into them.
void TransformChanged(ScriptContext& ctx, const ScriptEntity& handle)
{
    const Entity& entity = ResolveEntity(ctx, handle);
    ctx.host.scene->ApplyEntityTransform(entity);
}

const char* EntityKind(ScriptContext& ctx, const Entity& entity)
{
    if (entity.GetComponent<ActorComponent>())             return "actor";
    if (entity.GetComponent<MeshRendererComponent>())      return "mesh";
    if (entity.GetComponent<PrimitiveRendererComponent>()) return "primitive";
    if (entity.GetComponent<PointLightComponent>())        return "point_light";
    if (entity.GetComponent<ParticleEmitterComponent>())   return "particle_emitter";
    if (entity.GetComponent<CameraComponent>())            return "camera";
    if (entity.GetComponent<DirectionalLightComponent>())  return "sun";
    if (entity.GetComponent<RigidBodyComponent>())         return "rigid_body";
    if (ctx.host.scene->FindActorPart(entity.GetID()))     return "actor_component";
    return "empty";
}

// --- Actors ----------------------------------------------------------------------

SceneRuntime::ActorPart& ResolvePart(ScriptContext& ctx, const ScriptActorPart& handle,
                                     SceneRuntime::LoadedActor** actor = nullptr)
{
    const Entity& entity = ResolveEntity(ctx, handle.entity);
    SceneRuntime::ActorPart* part = ctx.host.scene->FindActorPart(entity.GetID(), actor);
    if (!part)
        Raise("actor component '%s' no longer exists", entity.GetName().c_str());
    return *part;
}

SceneRuntime::ActorPart& ResolvePart(ScriptContext& ctx, const ScriptActorPart& handle, ActorComponentType type,
                                     const char* field)
{
    SceneRuntime::ActorPart& part = ResolvePart(ctx, handle);
    if (part.def.type != type)
        Raise("fox.ActorComponent.%s: '%s' is a %s component, not a %s", field, part.def.name.c_str(),
              ActorComponentTypeName(part.def.type), ActorComponentTypeName(type));
    return part;
}

void UpdatePartLight(SceneRuntime::ActorPart& part)
{
    const auto& def = part.def;
    part.light.color = { def.color[0] * def.intensity, def.color[1] * def.intensity, def.color[2] * def.intensity };
    part.light.radius = def.radius;
}

// The root entity id of the actor an actor instance, actor entity or part entity belongs to.
uint32_t ActorRoot(ScriptContext& ctx, const sol::object& target, const char* api)
{
    sol::object entityObject = target;
    if (target.get_type() == sol::type::table)
        entityObject = target.as<sol::table>().raw_get<sol::object>("entity");
    if (!entityObject.is<ScriptEntity>())
        Raise("%s expects an actor (or one of its entities)", api);
    const Entity& entity = ResolveEntity(ctx, entityObject.as<ScriptEntity>());
    if (entity.GetComponent<ActorComponent>())
        return entity.GetID();
    SceneRuntime::LoadedActor* owner = nullptr;
    if (ctx.host.scene->FindActorPart(entity.GetID(), &owner))
        return owner->entity->GetID();
    Raise("%s: entity '%s' is not an actor", api, entity.GetName().c_str());
}

// Whether an actor instance's class is cls (a class table or its script path) or derives from it.
bool IsInstanceOf(sol::table self, const sol::object& cls)
{
    std::string path;
    if (cls.get_type() == sol::type::string)
    {
        path = cls.as<std::string>();
        std::replace(path.begin(), path.end(), '\\', '/');
    }
    sol::object current = self[sol::metatable_key];
    while (current.get_type() == sol::type::table)
    {
        const sol::table table = current.as<sol::table>();
        if (path.empty() ? table.pointer() == cls.pointer()
                         : table.raw_get<sol::object>("__path").is<std::string>() &&
                               table.raw_get<std::string>("__path") == path)
            return true;
        current = table.raw_get<sol::object>("__base");
    }
    return false;
}

std::string EntityLabel(const ScriptContext& ctx, const ScriptEntity& handle)
{
    char text[160];
    if (!IsLive(ctx, handle))
    {
        snprintf(text, sizeof(text), "fox.Entity(%u, unloaded)", handle.id);
        return text;
    }
    const Entity* entity = ctx.host.scene->world.GetScene().FindEntity(handle.id);
    snprintf(text, sizeof(text), "fox.Entity(%u \"%s\")", handle.id, entity->GetName().c_str());
    return text;
}

// --- Math ----------------------------------------------------------------------

XMFLOAT3 Normalized(const XMFLOAT3& v, const char* api)
{
    const XMVECTOR vector = XMLoadFloat3(&v);
    if (XMVectorGetX(XMVector3LengthSq(vector)) < 1e-12f)
        Raise("%s: direction must not be zero", api);
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(vector));
    return out;
}

ScriptVec3 WorldPosition(const TransformComponent& t)
{
    XMFLOAT3 position;
    XMStoreFloat3(&position, t.GetWorldMatrix().r[3]);
    return FromFloat3(position);
}

// Row-vector matrices: rows 0..2 are the world-space right, up and forward axes.
ScriptVec3 WorldAxis(const TransformComponent& t, int row)
{
    XMFLOAT3 axis;
    XMStoreFloat3(&axis, XMVector3Normalize(t.GetWorldMatrix().r[row]));
    return FromFloat3(axis);
}

// World-space point or direction into the transform's parent space.
XMVECTOR ToParentSpace(const TransformComponent& t, XMVECTOR value, bool point)
{
    if (!t.parent)
        return value;
    const XMMATRIX inverse = XMMatrixInverse(nullptr, t.parent->GetWorldMatrix());
    return point ? XMVector3TransformCoord(value, inverse) : XMVector3TransformNormal(value, inverse);
}

// Yaw/pitch (degrees) of a direction for the engine's euler order: yaw turns
// +Z towards +X, positive transform pitch looks down.
void DirectionAngles(const XMFLOAT3& d, double& yawDeg, double& pitchDownDeg)
{
    const double horizontal = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.z) * d.z);
    yawDeg = std::atan2(d.x, d.z) * k_radToDeg;
    pitchDownDeg = std::atan2(-static_cast<double>(d.y), horizontal) * k_radToDeg;
}

// --- Camera --------------------------------------------------------------------

CameraComponent& RequireCamera(ScriptContext& ctx)
{
    return *RequireScene(ctx, "fox.camera").camera;
}

XMFLOAT3 CameraForward(const CameraComponent& camera)
{
    const XMVECTOR forward = XMVectorSubtract(XMLoadFloat3(&camera.target), XMLoadFloat3(&camera.eye));
    if (XMVectorGetX(XMVector3LengthSq(forward)) < 1e-12f)
        return { 0.0f, 0.0f, 1.0f };
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(forward));
    return out;
}

double CameraYaw(const CameraComponent& camera)
{
    const XMFLOAT3 f = CameraForward(camera);
    return std::atan2(f.x, f.z) * k_radToDeg;
}

double CameraPitch(const CameraComponent& camera)
{
    const XMFLOAT3 f = CameraForward(camera);
    return std::asin(std::clamp(static_cast<double>(f.y), -1.0, 1.0)) * k_radToDeg;
}

// Camera convention: yaw clockwise from +Z seen from above, pitch up from the horizon.
void SetCameraAngles(CameraComponent& camera, double yawDeg, double pitchDeg)
{
    if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg))
        Raise("fox.camera: yaw and pitch must be finite numbers");
    const double yaw = yawDeg * k_degToRad;
    const double pitch = std::clamp(pitchDeg, -k_maxPitchDeg, k_maxPitchDeg) * k_degToRad;
    camera.target = {
        camera.eye.x + static_cast<float>(std::sin(yaw) * std::cos(pitch)),
        camera.eye.y + static_cast<float>(std::sin(pitch)),
        camera.eye.z + static_cast<float>(std::cos(yaw) * std::cos(pitch)),
    };
    camera.up = { 0.0f, 1.0f, 0.0f };
}

XMMATRIX CameraViewProj(const ScriptContext& ctx, const CameraComponent& camera, const char* api)
{
    if (ctx.view.width <= 0.0f || ctx.view.height <= 0.0f)
        Raise("%s: the game view has no size yet", api);
    return camera.GetViewMatrix() * camera.GetProjectionMatrix(ctx.view.width / ctx.view.height);
}

// --- Graphics ------------------------------------------------------------------

struct AAMode
{
    const char* name;
    bool fsr;
    int fsrQuality;
    bool taa;
    bool fxaa;
};

constexpr AAMode k_aaModes[] = {
    { "off",             false, 3, false, false },
    { "fxaa",            false, 3, false, true  },
    { "taa",             false, 3, true,  false },
    { "fsr_native",      true,  3, false, false },
    { "fsr_quality",     true,  0, false, false },
    { "fsr_balanced",    true,  1, false, false },
    { "fsr_performance", true,  2, false, false },
};

const char* CurrentAAMode(const SceneRuntime& scene)
{
    if (scene.fsrEnabled)
    {
        for (const AAMode& mode : k_aaModes)
            if (mode.fsr && mode.fsrQuality == scene.fsrQuality)
                return mode.name;
        return "fsr_native";
    }
    if (scene.taa.enabled) return "taa";
    if (scene.fxaa.enabled) return "fxaa";
    return "off";
}

void SetAAMode(SceneRuntime& scene, const std::string& name)
{
    for (const AAMode& mode : k_aaModes)
    {
        if (name != mode.name)
            continue;
        // FSR, TAA and FXAA are mutually exclusive paths.
        scene.fsrEnabled = mode.fsr;
        if (mode.fsr)
            scene.fsrQuality = mode.fsrQuality;
        scene.taa.enabled = mode.taa;
        scene.fxaa.enabled = mode.fxaa;
        scene.taa.ResetHistory();
        return;
    }
    Raise("unknown anti-aliasing mode '%s' (off, fxaa, taa, fsr_native, fsr_quality, fsr_balanced, fsr_performance)",
          name.c_str());
}

float Clamped(double value, double min, double max, const char* name)
{
    if (!std::isfinite(value))
        Raise("%s must be a finite number", name);
    return static_cast<float>(std::clamp(value, min, max));
}

} // namespace

void BindScene(ScriptApi& api, ScriptContext& ctx)
{
    // --- fox.Entity -----------------------------------------------------------

    api.Class<ScriptEntity>("Entity",
        "Handle to an entity of the loaded scene. Handles stay valid until the\n"
        "scene changes; using one afterwards raises an error (check is_valid).",
        sol::no_constructor)
        .ReadOnly("id", "integer", "Runtime id, unique within the loaded scene.",
            [](const ScriptEntity& self) { return static_cast<lua_Integer>(self.id); })
        .ReadOnly("source_id", "integer", "Id in the scene file (0 for entities created at runtime).",
            [&ctx](const ScriptEntity& self) {
                const auto* identity = ResolveEntity(ctx, self).GetComponent<LevelIdentityComponent>();
                return static_cast<lua_Integer>(identity ? identity->sourceID : 0);
            })
        .ReadOnly("name", "string", "",
            [&ctx](const ScriptEntity& self) { return ResolveEntity(ctx, self).GetName(); })
        .ReadOnly("kind", "string",
            "actor, actor_component, mesh, primitive, point_light, particle_emitter, camera, sun,\n"
            "rigid_body or empty.",
            [&ctx](const ScriptEntity& self) { return std::string(EntityKind(ctx, ResolveEntity(ctx, self))); })
        .ReadOnly("actor", "fox.Actor?",
            "The actor this entity is the root or a component of (its class instance), if any.",
            [&ctx](const ScriptEntity& self, sol::this_state s) -> sol::object {
                const Entity& entity = ResolveEntity(ctx, self);
                SceneRuntime::LoadedActor* owner = nullptr;
                if (entity.GetComponent<ActorComponent>())
                    return ctx.instances->ActorInstance(s, entity.GetID());
                if (ctx.host.scene->FindActorPart(entity.GetID(), &owner))
                    return ctx.instances->ActorInstance(s, owner->entity->GetID());
                return sol::make_object(s, sol::lua_nil);
            })
        .Property("active", "boolean",
            "Inactive entities are hidden and their scripts receive no updates\n"
            "(on_disable/on_enable run on the transitions). Not inherited by children.",
            [&ctx](const ScriptEntity& self) { return ResolveEntity(ctx, self).active; },
            [&ctx](const ScriptEntity& self, bool value) {
                Entity& entity = ResolveEntity(ctx, self);
                ctx.host.scene->SetEntityActive(entity, value);
            })
        .ReadOnly("transform", "fox.Transform", "",
            [&ctx](const ScriptEntity& self) {
                ResolveTransform(ctx, self);
                return ScriptTransform{ self };
            })
        .ReadOnly("parent", "fox.Entity?", "Parent entity, if any.",
            [&ctx](const ScriptEntity& self, sol::this_state s) {
                const TransformComponent& transform = ResolveTransform(ctx, self);
                return EntityOrNil(s, ctx, transform.parent ? transform.parent->GetOwner() : nullptr);
            })
        .Method("children", "() -> fox.Entity[]", "Direct children in scene order.",
            [&ctx](const ScriptEntity& self, sol::this_state s) {
                sol::state_view lua(s);
                sol::table result = lua.create_table();
                int index = 1;
                for (const TransformComponent* child : ResolveTransform(ctx, self).children)
                    if (child->GetOwner())
                        result[index++] = MakeEntityHandle(ctx, child->GetOwner()->GetID());
                return result;
            })
        .Method("is_valid", "() -> boolean", "False once the entity or its scene is gone.",
            [&ctx](const ScriptEntity& self) { return IsLive(ctx, self); })
        .Method("get_script", "(path: string) -> fox.Script?",
            "The instance of the script at path on this entity (\"Assets/Scripts/Door.lua\").",
            [&ctx](const ScriptEntity& self, const std::string& path, sol::this_state s) {
                ResolveEntity(ctx, self);
                return ctx.instances->FindScript(s, self.id, path);
            })
        .Method("add_script", "(path: string, properties?: table) -> fox.Script",
            "Attaches a script component at runtime (not saved). Its on_start runs next frame.",
            [&ctx](const ScriptEntity& self, const std::string& path, sol::object properties, sol::this_state s) {
                ResolveEntity(ctx, self);
                return ctx.instances->AddScript(s, self.id, path, properties);
            })
        .Operator(sol::meta_function::equal_to, "",
            [](const ScriptEntity& a, const ScriptEntity& b) { return a.id == b.id && a.generation == b.generation; })
        .Operator(sol::meta_function::to_string, "",
            [&ctx](const ScriptEntity& self) { return EntityLabel(ctx, self); });

    // --- fox.ActorComponent -------------------------------------------------------

    api.Class<ScriptActorPart>("ActorComponent",
        "One component of an actor (self.<name> on the actor), declared in its\n"
        "fox.actor class. Each is a child entity of the actor: its transform is\n"
        "relative to the actor, and moving it moves what it renders and its\n"
        "kinematic collider. Type-specific fields raise on other types.",
        sol::no_constructor)
        .ReadOnly("name", "string", "Component name from the class.",
            [&ctx](const ScriptActorPart& self) { return ResolvePart(ctx, self).def.name; })
        .ReadOnly("type", "string", "mesh, point_light, particle_emitter, box or sphere.",
            [&ctx](const ScriptActorPart& self) { return std::string(ActorComponentTypeName(ResolvePart(ctx, self).def.type)); })
        .ReadOnly("entity", "fox.Entity", "The component's child entity.",
            [&ctx](const ScriptActorPart& self) {
                ResolvePart(ctx, self);
                return self.entity;
            })
        .ReadOnly("transform", "fox.Transform", "Transform relative to the actor.",
            [&ctx](const ScriptActorPart& self) {
                ResolvePart(ctx, self);
                return ScriptTransform{ self.entity };
            })
        .ReadOnly("actor", "fox.Actor", "The owning actor.",
            [&ctx](const ScriptActorPart& self, sol::this_state s) {
                SceneRuntime::LoadedActor* actor = nullptr;
                ResolvePart(ctx, self, &actor);
                return ctx.instances->ActorInstance(s, actor->entity->GetID());
            })
        .Property("enabled", "boolean",
            "Disabled components render nothing, emit nothing and have no collision.",
            [&ctx](const ScriptActorPart& self) {
                ResolvePart(ctx, self);
                return ResolveEntity(ctx, self.entity).active;
            },
            [&ctx](const ScriptActorPart& self, bool value) {
                ResolvePart(ctx, self);
                ctx.host.scene->SetEntityActive(ResolveEntity(ctx, self.entity), value);
            })
        .Property("color", "fox.Vec3", "point_light: linear RGB. box/sphere: albedo, 0..1 per channel.",
            [&ctx](const ScriptActorPart& self) {
                const auto& part = ResolvePart(ctx, self);
                if (part.def.type != ActorComponentType::PointLight && !part.def.IsCollider())
                    Raise("fox.ActorComponent.color: '%s' is a %s component, not a point_light, box or sphere",
                          part.def.name.c_str(), ActorComponentTypeName(part.def.type));
                const auto& c = part.def.color;
                return ScriptVec3{ c[0], c[1], c[2] };
            },
            [&ctx](const ScriptActorPart& self, const ScriptVec3& value) {
                auto& part = ResolvePart(ctx, self);
                if (part.def.type == ActorComponentType::PointLight)
                {
                    part.def.color = { Clamped(value.x, 0.0, 1e4, "color.x"), Clamped(value.y, 0.0, 1e4, "color.y"),
                                       Clamped(value.z, 0.0, 1e4, "color.z") };
                    UpdatePartLight(part);
                    return;
                }
                if (!part.def.IsCollider())
                    Raise("fox.ActorComponent.color: '%s' is a %s component, not a point_light, box or sphere",
                          part.def.name.c_str(), ActorComponentTypeName(part.def.type));
                part.def.color = { Clamped(value.x, 0.0, 1.0, "color.x"), Clamped(value.y, 0.0, 1.0, "color.y"),
                                   Clamped(value.z, 0.0, 1.0, "color.z") };
                for (auto& material : part.materials)
                    material.tint = { part.def.color[0], part.def.color[1], part.def.color[2] };
            })
        .Property("visible", "boolean", "box/sphere: false keeps the collider but renders nothing.",
            [&ctx](const ScriptActorPart& self) {
                const auto& part = ResolvePart(ctx, self);
                if (!part.def.IsCollider())
                    Raise("fox.ActorComponent.visible: '%s' is a %s component, not a box or sphere",
                          part.def.name.c_str(), ActorComponentTypeName(part.def.type));
                return part.def.visible;
            },
            [&ctx](const ScriptActorPart& self, bool value) {
                auto& part = ResolvePart(ctx, self);
                if (!part.def.IsCollider())
                    Raise("fox.ActorComponent.visible: '%s' is a %s component, not a box or sphere",
                          part.def.name.c_str(), ActorComponentTypeName(part.def.type));
                part.def.visible = value;
            })
        .Property("intensity", "number", "point_light: multiplier on color.",
            [&ctx](const ScriptActorPart& self) {
                return static_cast<double>(ResolvePart(ctx, self, ActorComponentType::PointLight, "intensity").def.intensity);
            },
            [&ctx](const ScriptActorPart& self, double value) {
                auto& part = ResolvePart(ctx, self, ActorComponentType::PointLight, "intensity");
                part.def.intensity = Clamped(value, 0.0, 1e4, "intensity");
                UpdatePartLight(part);
            })
        .Property("radius", "number", "point_light: range in m.",
            [&ctx](const ScriptActorPart& self) {
                return static_cast<double>(ResolvePart(ctx, self, ActorComponentType::PointLight, "radius").def.radius);
            },
            [&ctx](const ScriptActorPart& self, double value) {
                auto& part = ResolvePart(ctx, self, ActorComponentType::PointLight, "radius");
                part.def.radius = Clamped(value, 0.01, 1e4, "radius");
                UpdatePartLight(part);
            })
        .Property("velocity", "fox.Vec3",
            "box/sphere with body = \"dynamic\": linear velocity of the actor in m/s.",
            [&ctx](const ScriptActorPart& self) {
                SceneRuntime::LoadedActor* actor = nullptr;
                const auto& part = ResolvePart(ctx, self, &actor);
                if (&part - actor->parts.data() != actor->dynamicPart)
                    Raise("fox.ActorComponent.velocity: '%s' is not the actor's dynamic body", part.def.name.c_str());
                return FromFloat3(actor->entity->GetComponent<RigidBodyComponent>()->velocity);
            },
            [&ctx](const ScriptActorPart& self, const ScriptVec3& value) {
                SceneRuntime::LoadedActor* actor = nullptr;
                const auto& part = ResolvePart(ctx, self, &actor);
                if (&part - actor->parts.data() != actor->dynamicPart)
                    Raise("fox.ActorComponent.velocity: '%s' is not the actor's dynamic body", part.def.name.c_str());
                actor->entity->GetComponent<RigidBodyComponent>()->velocity = ToFloat3(value);
            })
        .Method("emit", "(count: integer) -> nil", "particle_emitter: emits a burst of particles next frame.",
            [&ctx](const ScriptActorPart& self, lua_Integer count) {
                auto& part = ResolvePart(ctx, self, ActorComponentType::ParticleEmitter, "emit");
                if (count < 0 || count > 100000)
                    Raise("fox.ActorComponent.emit: count must be in 0..100000");
                if (part.particles)
                    part.particles->Burst(static_cast<int>(count));
            })
        .Operator(sol::meta_function::equal_to, "",
            [](const ScriptActorPart& a, const ScriptActorPart& b) {
                return a.entity.id == b.entity.id && a.entity.generation == b.entity.generation;
            })
        .Operator(sol::meta_function::to_string, "",
            [&ctx](const ScriptActorPart& self) { return "fox.ActorComponent of " + EntityLabel(ctx, self.entity); });

    // --- fox.Transform ----------------------------------------------------------

    api.Class<ScriptTransform>("Transform",
        "Position, rotation and scale of an entity, relative to its parent.\n"
        "Rotation is euler degrees: x = pitch (positive looks down), y = yaw\n"
        "(positive turns +Z towards +X), z = roll. Values are copies: assign\n"
        "the modified vector back. Moving a light, emitter or primitive entity\n"
        "moves what it renders; the scene camera is driven by fox.camera.",
        sol::no_constructor)
        .Property("position", "fox.Vec3", "Local position.",
            [&ctx](const ScriptTransform& self) { return FromFloat3(ResolveTransform(ctx, self.entity).position); },
            [&ctx](const ScriptTransform& self, const ScriptVec3& value) {
                ResolveTransform(ctx, self.entity).position = ToFloat3(value);
                TransformChanged(ctx, self.entity);
            })
        .Property("rotation", "fox.Vec3", "Local euler angles in degrees.",
            [&ctx](const ScriptTransform& self) { return FromFloat3(ResolveTransform(ctx, self.entity).eulerDeg); },
            [&ctx](const ScriptTransform& self, const ScriptVec3& value) {
                ResolveTransform(ctx, self.entity).eulerDeg = ToFloat3(value);
                TransformChanged(ctx, self.entity);
            })
        .Property("scale", "fox.Vec3", "Local scale.",
            [&ctx](const ScriptTransform& self) { return FromFloat3(ResolveTransform(ctx, self.entity).scale); },
            [&ctx](const ScriptTransform& self, const ScriptVec3& value) {
                ResolveTransform(ctx, self.entity).scale = ToFloat3(value);
                TransformChanged(ctx, self.entity);
            })
        .Property("world_position", "fox.Vec3", "World-space position.",
            [&ctx](const ScriptTransform& self) { return WorldPosition(ResolveTransform(ctx, self.entity)); },
            [&ctx](const ScriptTransform& self, const ScriptVec3& value) {
                TransformComponent& transform = ResolveTransform(ctx, self.entity);
                const XMFLOAT3 world = ToFloat3(value);
                XMStoreFloat3(&transform.position, ToParentSpace(transform, XMLoadFloat3(&world), true));
                TransformChanged(ctx, self.entity);
            })
        .ReadOnly("forward", "fox.Vec3", "World-space +Z axis.",
            [&ctx](const ScriptTransform& self) { return WorldAxis(ResolveTransform(ctx, self.entity), 2); })
        .ReadOnly("right", "fox.Vec3", "World-space +X axis.",
            [&ctx](const ScriptTransform& self) { return WorldAxis(ResolveTransform(ctx, self.entity), 0); })
        .ReadOnly("up", "fox.Vec3", "World-space +Y axis.",
            [&ctx](const ScriptTransform& self) { return WorldAxis(ResolveTransform(ctx, self.entity), 1); })
        .ReadOnly("entity", "fox.Entity", "The owning entity.",
            [](const ScriptTransform& self) { return self.entity; })
        .Method("translate", "(delta: fox.Vec3) -> nil", "Moves by delta in parent space.",
            [&ctx](const ScriptTransform& self, const ScriptVec3& delta) {
                TransformComponent& transform = ResolveTransform(ctx, self.entity);
                transform.position.x += static_cast<float>(delta.x);
                transform.position.y += static_cast<float>(delta.y);
                transform.position.z += static_cast<float>(delta.z);
                TransformChanged(ctx, self.entity);
            })
        .Method("rotate", "(degrees: fox.Vec3) -> nil", "Adds euler degrees to the rotation.",
            [&ctx](const ScriptTransform& self, const ScriptVec3& degrees) {
                TransformComponent& transform = ResolveTransform(ctx, self.entity);
                transform.eulerDeg.x = static_cast<float>(std::remainder(transform.eulerDeg.x + degrees.x, 360.0));
                transform.eulerDeg.y = static_cast<float>(std::remainder(transform.eulerDeg.y + degrees.y, 360.0));
                transform.eulerDeg.z = static_cast<float>(std::remainder(transform.eulerDeg.z + degrees.z, 360.0));
                TransformChanged(ctx, self.entity);
            })
        .Method("look_at", "(target: fox.Vec3) -> nil", "Turns +Z towards a world-space point (roll 0).",
            [&ctx](const ScriptTransform& self, const ScriptVec3& target) {
                TransformComponent& transform = ResolveTransform(ctx, self.entity);
                const XMFLOAT3 world = ToFloat3(target);
                const XMVECTOR direction = XMVectorSubtract(XMLoadFloat3(&world), transform.GetWorldMatrix().r[3]);
                XMFLOAT3 local;
                XMStoreFloat3(&local, ToParentSpace(transform, direction, false));
                if (std::abs(local.x) + std::abs(local.y) + std::abs(local.z) < 1e-6f)
                    return;
                double yaw = 0.0, pitch = 0.0;
                DirectionAngles(local, yaw, pitch);
                transform.eulerDeg = { static_cast<float>(pitch), static_cast<float>(yaw), 0.0f };
                TransformChanged(ctx, self.entity);
            })
        .Operator(sol::meta_function::equal_to, "",
            [](const ScriptTransform& a, const ScriptTransform& b) {
                return a.entity.id == b.entity.id && a.entity.generation == b.entity.generation;
            })
        .Operator(sol::meta_function::to_string, "",
            [&ctx](const ScriptTransform& self) { return "fox.Transform of " + EntityLabel(ctx, self.entity); });

    // --- fox.scene ----------------------------------------------------------------

    ApiDocs::Block& info = api.DocClass("fox.SceneInfo", "Argument of on_scene_loaded.");
    info.fields = {
        { "name", "string", "Scene name from the level file." },
        { "path", "string", "Project-relative scene path." },
    };

    api.Module("scene",
        "The loaded scene. Loading is asynchronous: load() starts it, scripts keep\n"
        "running until the new scene is ready, then entity scripts of the old scene\n"
        "get on_destroy, the new scene's scripts are created, and autoloads receive\n"
        "on_scene_loaded. Scene loads reset per-scene settings (sun, fog, bloom,\n"
        "exposure), so re-apply player preferences in on_scene_loaded.")
        .Function("name", "() -> string", "Name of the loaded scene.",
            [&ctx] { return RequireScene(ctx, "fox.scene").currentDesc.name; })
        .Function("path", "() -> string", "Project-relative path of the loaded scene.",
            [&ctx] { return RequireScene(ctx, "fox.scene").currentScenePath; })
        .Function("list", "() -> string[]",
            "Scenes of the project: project.json \"scenes\", or every scene in Assets/Scenes.",
            [&ctx](sol::this_state s) {
                const std::vector<std::string>& scenes = ctx.host.scenes;
                sol::state_view lua(s);
                sol::table result = lua.create_table(static_cast<int>(scenes.size()), 0);
                for (size_t i = 0; i < scenes.size(); ++i)
                    result[i + 1] = scenes[i];
                return result;
            })
        .Function("load", "(path: string) -> nil", "Starts loading a scene (\"Assets/Scenes/sponza.json\").",
            [&ctx](const std::string& path) {
                SceneRuntime& scene = RequireRuntime(ctx, "fox.scene.load");
                std::error_code ec;
                if (!std::filesystem::is_regular_file(path, ec))
                    Raise("fox.scene.load: scene '%s' does not exist", path.c_str());
                scene.RequestSceneLoad(path);
            })
        .Function("reload", "() -> nil", "Reloads the current scene from its start state.",
            [&ctx] {
                SceneRuntime& scene = RequireScene(ctx, "fox.scene.reload");
                // Editor Play mode loads its scene from in-memory JSON; reload that state.
                const bool fromMemory = scene.lastRequestedScenePath == scene.currentScenePath;
                scene.RequestSceneLoad(scene.currentScenePath, fromMemory ? scene.lastRequestedSceneJson : std::string());
            })
        .Function("is_loading", "() -> boolean", "True while a scene load is in progress.",
            [&ctx] { return RequireRuntime(ctx, "fox.scene.is_loading").IsLoading(); })
        .Function("find", "(name: string) -> fox.Entity?", "First entity with this name, in scene order.",
            [&ctx](const std::string& name, sol::this_state s) {
                return EntityOrNil(s, ctx, RequireScene(ctx, "fox.scene.find").world.GetScene().FindEntity(name));
            })
        .Function("find_all", "(name?: string) -> fox.Entity[]", "Every entity with this name (all entities without one).",
            [&ctx](sol::optional<std::string> name, sol::this_state s) {
                sol::state_view lua(s);
                sol::table result = lua.create_table();
                int index = 1;
                for (const auto& entity : RequireScene(ctx, "fox.scene.find_all").world.GetScene().GetEntities())
                    if (!name || entity->GetName() == *name)
                        result[index++] = MakeEntityHandle(ctx, entity->GetID());
                return result;
            })
        .Function("get", "(source_id: integer) -> fox.Entity?", "The entity with this id in the scene file.",
            [&ctx](lua_Integer sourceId, sol::this_state s) {
                SceneRuntime& scene = RequireScene(ctx, "fox.scene.get");
                return EntityOrNil(s, ctx, sourceId > 0 ? scene.world.FindBySourceID(static_cast<LevelEntityID>(sourceId))
                                                        : nullptr);
            });

    // --- fox.world ----------------------------------------------------------------

    ApiDocs::Block& spawnOptions = api.DocClass("fox.SpawnOptions", "Options of fox.world.spawn.");
    spawnOptions.fields = {
        { "position", "fox.Vec3?", "World position (default origin)." },
        { "rotation", "fox.Vec3?", "Euler degrees." },
        { "scale", "fox.Vec3?", "" },
        { "name", "string?", "Entity name (default: the class file name)." },
        { "properties", "table?", "Property overrides, like a level placement's." },
        { "components", "table<string, table>?",
          "Component overrides keyed by component name: { mesh = { mesh = \"Assets/Props/Crate2.fbx\" } }." },
    };

    api.Module("world",
        "Actors of the loaded scene: instances of fox.actor classes, placed in the\n"
        "level or spawned at runtime. Spawned actors are not saved and end with the scene.")
        .Function("spawn", "(class: fox.Actor|string, options?: fox.SpawnOptions) -> fox.Actor",
            "Creates an actor from its class (or script path) and returns its instance;\n"
            "on_start runs next frame. Meshes the level did not load are imported now\n"
            "(a hitch): place one instance in the level to preload a class's assets.",
            [&ctx](const sol::object& actorClass, const sol::object& options, sol::this_state s) {
                RequireScene(ctx, "fox.world.spawn");
                return ctx.instances->SpawnActor(s, actorClass, options);
            })
        .Function("destroy", "(actor: fox.Actor|fox.Entity) -> nil",
            "Destroys an actor at the end of this frame's update (its scripts get\n"
            "on_disable/on_destroy then). Same as actor:destroy().",
            [&ctx](const sol::object& target) {
                RequireScene(ctx, "fox.world.destroy");
                ctx.instances->DestroyActor(ActorRoot(ctx, target, "fox.world.destroy"));
            })
        .Function("actors", "(class?: fox.Actor|string) -> fox.Actor[]",
            "Every actor in creation order, or those of a class (or its script path) and its subclasses.",
            [&ctx](const sol::object& actorClass, sol::this_state s) {
                RequireScene(ctx, "fox.world.actors");
                sol::table all = ctx.instances->ActorInstances(s);
                const sol::type type = actorClass.get_type();
                if (type == sol::type::lua_nil || type == sol::type::none)
                    return all;
                if (type != sol::type::table && type != sol::type::string)
                    Raise("fox.world.actors: class must be an actor class or its script path");
                sol::state_view lua(s);
                sol::table result = lua.create_table();
                int index = 1;
                for (size_t i = 1; i <= all.size(); ++i)
                {
                    const sol::table self = all.raw_get<sol::table>(i);
                    if (IsInstanceOf(self, actorClass))
                        result[index++] = self;
                }
                return result;
            })
        .Function("find", "(name: string) -> fox.Actor?", "The first actor whose entity has this name.",
            [&ctx](const std::string& name, sol::this_state s) -> sol::object {
                SceneRuntime& scene = RequireScene(ctx, "fox.world.find");
                for (const SceneRuntime::LoadedActor& actor : scene.actors)
                    if (actor.entity->GetName() == name)
                    {
                        sol::object self = ctx.instances->ActorInstance(s, actor.entity->GetID());
                        if (self.get_type() == sol::type::table)
                            return self;
                    }
                return sol::make_object(s, sol::lua_nil);
            });

    // --- fox.camera ---------------------------------------------------------------

    api.Module("camera",
        "The scene camera that renders the game view. Yaw turns clockwise from +Z\n"
        "seen from above; pitch is degrees above the horizon (clamped to +-89.9).")
        .Property("position", "fox.Vec3", "Eye position; moving keeps the look direction.",
            [&ctx] { return FromFloat3(RequireCamera(ctx).eye); },
            [&ctx](const ScriptVec3& value) {
                CameraComponent& camera = RequireCamera(ctx);
                const XMFLOAT3 forward = CameraForward(camera);
                camera.eye = ToFloat3(value);
                camera.target = { camera.eye.x + forward.x, camera.eye.y + forward.y, camera.eye.z + forward.z };
            })
        .ReadOnly("forward", "fox.Vec3", "Unit look direction.",
            [&ctx] { return FromFloat3(CameraForward(RequireCamera(ctx))); })
        .ReadOnly("right", "fox.Vec3", "Unit right direction (horizontal).",
            [&ctx] {
                const double yaw = CameraYaw(RequireCamera(ctx)) * k_degToRad;
                return ScriptVec3{ std::cos(yaw), 0.0, -std::sin(yaw) };
            })
        .Property("yaw", "number", "Degrees.",
            [&ctx] { return CameraYaw(RequireCamera(ctx)); },
            [&ctx](double value) {
                CameraComponent& camera = RequireCamera(ctx);
                SetCameraAngles(camera, value, CameraPitch(camera));
            })
        .Property("pitch", "number", "Degrees above the horizon.",
            [&ctx] { return CameraPitch(RequireCamera(ctx)); },
            [&ctx](double value) {
                CameraComponent& camera = RequireCamera(ctx);
                SetCameraAngles(camera, CameraYaw(camera), value);
            })
        .Property("fov", "number", "Vertical field of view in degrees (1..170).",
            [&ctx] { return static_cast<double>(RequireCamera(ctx).fovDeg); },
            [&ctx](double value) { RequireCamera(ctx).fovDeg = Clamped(value, 1.0, 170.0, "fox.camera.fov"); })
        .Property("near", "number", "Near clip distance.",
            [&ctx] { return static_cast<double>(RequireCamera(ctx).nearZ); },
            [&ctx](double value) { RequireCamera(ctx).nearZ = Clamped(value, 1e-4, 1e6, "fox.camera.near"); })
        .Property("far", "number", "Far clip distance.",
            [&ctx] { return static_cast<double>(RequireCamera(ctx).farZ); },
            [&ctx](double value) { RequireCamera(ctx).farZ = Clamped(value, 1e-3, 1e7, "fox.camera.far"); })
        .Function("set_rotation", "(yaw: number, pitch: number) -> nil", "Sets the look direction in degrees.",
            [&ctx](double yaw, double pitch) { SetCameraAngles(RequireCamera(ctx), yaw, pitch); })
        .Function("look_at", "(target: fox.Vec3) -> nil", "Turns the camera towards a world-space point.",
            [&ctx](const ScriptVec3& target) {
                CameraComponent& camera = RequireCamera(ctx);
                const XMFLOAT3 point = ToFloat3(target);
                XMFLOAT3 direction;
                XMStoreFloat3(&direction, XMVectorSubtract(XMLoadFloat3(&point), XMLoadFloat3(&camera.eye)));
                if (std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z) < 1e-6f)
                    return;
                double yaw = 0.0, pitchDown = 0.0;
                DirectionAngles(direction, yaw, pitchDown);
                SetCameraAngles(camera, yaw, -pitchDown);
            })
        .Function("screen_ray", "(x: number, y: number) -> fox.Vec3, fox.Vec3",
            "World-space ray (origin, unit direction) through a game-view pixel, for picking.",
            [&ctx](double x, double y) {
                const CameraComponent& camera = RequireCamera(ctx);
                const XMMATRIX inverse = XMMatrixInverse(nullptr, CameraViewProj(ctx, camera, "fox.camera.screen_ray"));
                const Ray ray = Ray::FromNDC(static_cast<float>(x / ctx.view.width * 2.0 - 1.0),
                                             static_cast<float>(1.0 - y / ctx.view.height * 2.0), inverse);
                return std::make_tuple(FromFloat3(ray.origin), FromFloat3(ray.direction));
            })
        .Function("world_to_screen", "(point: fox.Vec3) -> number, number, boolean",
            "Game-view pixel of a world-space point, and whether it is on screen.",
            [&ctx](const ScriptVec3& point) {
                const CameraComponent& camera = RequireCamera(ctx);
                const XMMATRIX viewProj = CameraViewProj(ctx, camera, "fox.camera.world_to_screen");
                const XMFLOAT3 p = ToFloat3(point);
                XMFLOAT4 clip;
                XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.0f), viewProj));
                if (clip.w <= 1e-6f)
                    return std::make_tuple(0.0, 0.0, false);
                const double ndcX = clip.x / clip.w, ndcY = clip.y / clip.w, ndcZ = clip.z / clip.w;
                const double sx = (ndcX * 0.5 + 0.5) * ctx.view.width;
                const double sy = (0.5 - ndcY * 0.5) * ctx.view.height;
                const bool visible = std::abs(ndcX) <= 1.0 && std::abs(ndcY) <= 1.0 && ndcZ >= 0.0 && ndcZ <= 1.0;
                return std::make_tuple(sx, sy, visible);
            });

    // --- fox.character ------------------------------------------------------------

    api.Module("character",
        "The scene's walking capsule (feet at position): gravity, stair stepping and\n"
        "collision against every body; pushes dynamic bodies. Call move once per frame.")
        .Property("position", "fox.Vec3", "Feet position; assigning teleports.",
            [&ctx] { return FromFloat3(RequireScene(ctx, "fox.character").cc.position); },
            [&ctx](const ScriptVec3& value) { RequireScene(ctx, "fox.character").cc.position = ToFloat3(value); })
        .ReadOnly("eye_position", "fox.Vec3", "position + eye_height.",
            [&ctx] { return FromFloat3(RequireScene(ctx, "fox.character").cc.GetEyePosition()); })
        .ReadOnly("grounded", "boolean", "Touching ground after the last move.",
            [&ctx] { return RequireScene(ctx, "fox.character").cc.isGrounded; })
        .Property("vertical_velocity", "number", "Upward speed in m/s.",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.velY); },
            [&ctx](double value) {
                RequireScene(ctx, "fox.character").cc.velY = Clamped(value, -1e4, 1e4, "fox.character.vertical_velocity");
            })
        .Property("move_speed", "number", "Suggested walking speed in m/s (scripts scale input by it).",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.moveSpeed); },
            [&ctx](double value) {
                RequireScene(ctx, "fox.character").cc.moveSpeed = Clamped(value, 0.0, 1e4, "fox.character.move_speed");
            })
        .Property("jump_speed", "number", "Take-off speed of jump() in m/s.",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.jumpSpeed); },
            [&ctx](double value) {
                RequireScene(ctx, "fox.character").cc.jumpSpeed = Clamped(value, 0.0, 1e4, "fox.character.jump_speed");
            })
        .Property("eye_height", "number", "Eye height above the feet in m.",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.eyeHeight); },
            [&ctx](double value) {
                RequireScene(ctx, "fox.character").cc.eyeHeight = Clamped(value, 0.0, 100.0, "fox.character.eye_height");
            })
        .Property("gravity", "boolean", "Whether gravity pulls the character down.",
            [&ctx] { return RequireScene(ctx, "fox.character").cc.gravityEnabled; },
            [&ctx](bool value) { RequireScene(ctx, "fox.character").cc.gravityEnabled = value; })
        .ReadOnly("radius", "number", "Capsule radius in m.",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.radius); })
        .ReadOnly("height", "number", "Capsule height in m, including the end caps.",
            [&ctx] { return static_cast<double>(RequireScene(ctx, "fox.character").cc.height); })
        .ReadOnly("spawn_point", "fox.Vec3", "The scene's character spawn position.",
            [&ctx] {
                const auto& spawn = RequireScene(ctx, "fox.character").currentDesc.physics.characterSpawn;
                return ScriptVec3{ spawn[0], spawn[1], spawn[2] };
            })
        .Function("move", "(velocity: fox.Vec3, dt?: number) -> nil",
            "Moves for dt seconds (default fox.time.dt) at the horizontal velocity in m/s;\n"
            "velocity.y is ignored (gravity and jumps drive vertical motion).",
            [&ctx](const ScriptVec3& velocity, sol::optional<double> dt) {
                SceneRuntime& scene = RequireScene(ctx, "fox.character.move");
                const double step = dt.value_or(ctx.dt);
                if (!std::isfinite(step) || step <= 0.0)
                    return;
                scene.physicsWorld.StepCharacter(
                    scene.cc, { static_cast<float>(velocity.x), 0.0f, static_cast<float>(velocity.z) },
                    static_cast<float>(step));
            })
        .Function("jump", "() -> boolean", "Jumps off the ground; false (no jump) while airborne.",
            [&ctx] {
                CharacterController& cc = RequireScene(ctx, "fox.character.jump").cc;
                const bool grounded = cc.isGrounded;
                cc.Jump();
                return grounded;
            });

    // --- fox.physics --------------------------------------------------------------

    ApiDocs::Block& hit = api.DocClass("fox.RaycastHit", "Result of fox.physics.raycast.");
    hit.fields = {
        { "position", "fox.Vec3", "World-space hit point." },
        { "normal", "fox.Vec3", "Surface normal at the hit." },
        { "distance", "number", "Distance from the ray origin." },
        { "dynamic", "boolean", "True for simulated bodies." },
        { "entity", "fox.Entity?", "The hit entity, for simulated bodies." },
    };

    api.Module("physics", "Rigid-body world (Jolt). Simulation runs at a fixed rate; scripts get on_fixed_update per step.")
        .Function("raycast", "(origin: fox.Vec3, direction: fox.Vec3, max_distance?: number) -> fox.RaycastHit?",
            "Closest hit along the ray (ignores the character), or nil.",
            [&ctx](const ScriptVec3& origin, const ScriptVec3& direction, sol::optional<double> maxDistance,
                   sol::this_state s) -> sol::object {
                SceneRuntime& scene = RequireScene(ctx, "fox.physics.raycast");
                Ray ray;
                ray.origin = ToFloat3(origin);
                ray.direction = Normalized(ToFloat3(direction), "fox.physics.raycast");
                PhysicsWorld::RaycastHit result;
                const float distance = Clamped(maxDistance.value_or(10000.0), 0.0, 1e7, "max_distance");
                if (!scene.physicsWorld.Raycast(ray, result, distance))
                    return sol::lua_nil;
                sol::state_view lua(s);
                sol::table table = lua.create_table();
                table["position"] = FromFloat3(result.point);
                table["normal"] = FromFloat3(result.normal);
                table["distance"] = static_cast<double>(result.t);
                table["dynamic"] = result.dynamic;
                if (result.transform && result.transform->GetOwner())
                    table["entity"] = MakeEntityHandle(ctx, result.transform->GetOwner()->GetID());
                return table;
            })
        .Function("spawn_ball", "(position: fox.Vec3, radius?: number, velocity?: fox.Vec3) -> fox.Entity?",
            "Spawns a simulated sphere (default radius 0.5 m) that lives until remove_balls\n"
            "or the next scene load. Returns nil when the physics world is full.",
            [&ctx](const ScriptVec3& position, sol::optional<double> radius, sol::optional<ScriptVec3> velocity,
                   sol::this_state s) -> sol::object {
                SceneRuntime& scene = RequireScene(ctx, "fox.physics.spawn_ball");
                const float r = Clamped(radius.value_or(0.5), 0.01, 100.0, "radius");
                if (scene.SpawnBall(ToFloat3(position), r, ToFloat3(velocity.value_or(ScriptVec3{}))) == k_invalidBody)
                    return sol::lua_nil;
                return sol::make_object(s, MakeEntityHandle(ctx, scene.balls.back().entity));
            })
        .Function("remove_balls", "() -> nil", "Removes every ball spawned with spawn_ball.",
            [&ctx] { RequireScene(ctx, "fox.physics.remove_balls").RemoveBalls(); })
        .ReadOnly("gravity", "fox.Vec3", "Gravity acceleration in m/s^2.",
            [&ctx] { return FromFloat3(RequireRuntime(ctx, "fox.physics").physicsWorld.GetSettings().gravity); })
        .ReadOnly("fixed_dt", "number", "Seconds per physics step (the dt of on_fixed_update).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.physics").physicsWorld.GetSettings().fixedTimeStep); });

    // --- fox.graphics -------------------------------------------------------------

    api.DocAlias("fox.AAMode", "Anti-aliasing / upscaling path (mutually exclusive).",
        { "\"off\"", "\"fxaa\"", "\"taa\"", "\"fsr_native\" # FSR 3.1 native-resolution AA",
          "\"fsr_quality\"", "\"fsr_balanced\"", "\"fsr_performance\"" });

    api.Module("graphics", "Rendering settings of the scene runtime (readable and writable).")
        .Property("aa_mode", "fox.AAMode", "Anti-aliasing / upscaling mode.",
            [&ctx] { return std::string(CurrentAAMode(RequireRuntime(ctx, "fox.graphics"))); },
            [&ctx](const std::string& value) { SetAAMode(RequireRuntime(ctx, "fox.graphics"), value); })
        .Property("fsr_sharpening", "boolean", "FSR sharpening pass.",
            [&ctx] { return RequireRuntime(ctx, "fox.graphics").fsrSharpen; },
            [&ctx](bool value) { RequireRuntime(ctx, "fox.graphics").fsrSharpen = value; })
        .Property("fsr_sharpness", "number", "FSR sharpening strength (0..1).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.graphics").fsrSharpness); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.graphics").fsrSharpness = Clamped(value, 0.0, 1.0, "fox.graphics.fsr_sharpness");
            })
        .Property("bloom", "boolean", "Bloom (reset by scene loads).",
            [&ctx] { return RequireRuntime(ctx, "fox.graphics").bloom.enabled; },
            [&ctx](bool value) { RequireRuntime(ctx, "fox.graphics").bloom.enabled = value; })
        .Property("ambient_occlusion", "boolean", "Screen-space ambient occlusion.",
            [&ctx] { return RequireRuntime(ctx, "fox.graphics").ssao.enabled; },
            [&ctx](bool value) { RequireRuntime(ctx, "fox.graphics").ssao.enabled = value; })
        .Property("volumetric_fog", "boolean", "Volumetric fog and light shafts (reset by scene loads).",
            [&ctx] { return RequireRuntime(ctx, "fox.graphics").volFog.enabled; },
            [&ctx](bool value) { RequireRuntime(ctx, "fox.graphics").volFog.enabled = value; })
        .Property("fog_density", "number", "Volumetric fog density (reset by scene loads).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.graphics").volFog.density); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.graphics").volFog.density = Clamped(value, 0.0, 10.0, "fox.graphics.fog_density");
            })
        .Property("exposure", "number", "Tone-mapping exposure multiplier (reset by scene loads).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.graphics").toneMap.exposure); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.graphics").toneMap.exposure = Clamped(value, 0.0, 100.0, "fox.graphics.exposure");
            })
        .Property("saturation", "number", "Colour saturation (1 = neutral; reset by scene loads).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.graphics").toneMap.saturation); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.graphics").toneMap.saturation = Clamped(value, 0.0, 4.0, "fox.graphics.saturation");
            })
        .Property("vignette", "number", "Vignette strength (0..1; reset by scene loads).",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.graphics").toneMap.vignette); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.graphics").toneMap.vignette = Clamped(value, 0.0, 1.0, "fox.graphics.vignette");
            });

    // --- fox.lighting -------------------------------------------------------------

    api.Module("lighting", "Sun and ambient light of the scene (reset by scene loads).")
        .Property("sun_enabled", "boolean", "",
            [&ctx] { return RequireRuntime(ctx, "fox.lighting").lights.sunEnabled; },
            [&ctx](bool value) { RequireRuntime(ctx, "fox.lighting").lights.sunEnabled = value; })
        .Property("sun_elevation", "number", "Degrees above the horizon.",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.lighting").lights.elevDeg); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.lighting").lights.elevDeg = Clamped(value, -90.0, 90.0, "fox.lighting.sun_elevation");
            })
        .Property("sun_azimuth", "number", "Compass direction in degrees.",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.lighting").lights.azimDeg); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.lighting").lights.azimDeg =
                    static_cast<float>(std::remainder(Clamped(value, -1e6, 1e6, "fox.lighting.sun_azimuth"), 360.0));
            })
        .Property("sun_intensity", "number", "Multiplier on sun_color.",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.lighting").lights.lightIntensity); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.lighting").lights.lightIntensity = Clamped(value, 0.0, 1e4, "fox.lighting.sun_intensity");
            })
        .Property("sun_color", "fox.Vec3", "Linear RGB.",
            [&ctx] {
                const float* c = RequireRuntime(ctx, "fox.lighting").lights.lightColor;
                return ScriptVec3{ c[0], c[1], c[2] };
            },
            [&ctx](const ScriptVec3& value) {
                float* c = RequireRuntime(ctx, "fox.lighting").lights.lightColor;
                c[0] = Clamped(value.x, 0.0, 1e4, "fox.lighting.sun_color");
                c[1] = Clamped(value.y, 0.0, 1e4, "fox.lighting.sun_color");
                c[2] = Clamped(value.z, 0.0, 1e4, "fox.lighting.sun_color");
            })
        .Property("ambient_intensity", "number", "Image-based ambient light multiplier.",
            [&ctx] { return static_cast<double>(RequireRuntime(ctx, "fox.lighting").lights.iblIntensity); },
            [&ctx](double value) {
                RequireRuntime(ctx, "fox.lighting").lights.iblIntensity = Clamped(value, 0.0, 100.0, "fox.lighting.ambient_intensity");
            });
}

} // namespace SE::Scripting
