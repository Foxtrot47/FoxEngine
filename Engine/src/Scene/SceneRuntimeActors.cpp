#include "Engine/Scene/SceneRuntime.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>
#include "Engine/Assets/MeshCache.h"
#include "Engine/Core/Logger.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/MeshMaterialUtil.h"

using namespace DirectX;

namespace SE {

namespace {

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

bool IsActive(const TransformComponent* transform)
{
    return transform && transform->GetOwner() && transform->GetOwner()->active;
}

// World pose of a transform: position, rotation quaternion and scale.
void WorldPose(const TransformComponent& transform, XMFLOAT3& position, XMFLOAT4& rotation,
               XMFLOAT3& scale)
{
    XMVECTOR s, r, t;
    if (!XMMatrixDecompose(&s, &r, &t, transform.GetWorldMatrix()))
    {
        s = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
        r = XMQuaternionIdentity();
        t = transform.GetWorldMatrix().r[3];
    }
    XMStoreFloat3(&position, t);
    XMStoreFloat4(&rotation, r);
    XMStoreFloat3(&scale, XMVectorAbs(s));
}

XMFLOAT3 WorldPosition(const TransformComponent& transform)
{
    XMFLOAT3 position;
    XMStoreFloat3(&position, transform.GetWorldMatrix().r[3]);
    return position;
}

// Texture from the asset cache; decoded and uploaded now when missing.
AssetHandle<Texture2D> LoadTextureNow(AssetManager& assets, ID3D11Device* device, const std::string& path)
{
    const std::string normalized = LevelDependencyPlan::NormalizePath(path);
    if (normalized.empty())
        return {};
    if (auto cached = assets.FindTexture(Wide(normalized)))
        return cached;
    TextureSourceData source;
    if (!Texture2D::Decode(Wide(normalized).c_str(), source))
    {
        SE_LOG_WARN("Actor: texture '%s' could not be decoded", normalized.c_str());
        return {};
    }
    auto texture = std::make_shared<Texture2D>();
    if (!texture->Upload(device, source))
        return {};
    assets.CacheTexture(Wide(normalized), texture);
    return texture;
}

} // namespace

// --- Assets -------------------------------------------------------------------

bool SceneRuntime::LoadActorMesh(ActorPart& part, bool loadMissing)
{
    const std::string path = LevelDependencyPlan::NormalizePath(part.def.mesh);
    part.mesh = m_assets->FindMesh(path);
    if (part.def.collision)
        part.collider = m_assets->FindCollider(path);

    const bool needSource = !part.mesh || (part.def.collision && !part.collider);
    if (needSource && loadMissing)
    {
        // Runtime spawn of a class the level did not place: import now.
        MeshSourceData source;
        std::string error;
        const MeshCache cache;
        if (!cache.Load(path, source, {}, &error))
        {
            SE_LOG_WARN("Actor: mesh '%s' failed to load (%s)", path.c_str(), error.c_str());
            return false;
        }
        SE_LOG_INFO("Actor: imported '%s' synchronously (place the actor class in the level to preload it)",
                    path.c_str());
        if (part.def.collision && !part.collider)
        {
            part.collider = BuildLevelMeshCollider(source, &error);
            if (part.collider)
                m_assets->CacheCollider(path, part.collider);
            else
                SE_LOG_WARN("Actor: collision for '%s' not built (%s)", path.c_str(), error.c_str());
        }
        if (!part.mesh)
        {
            ID3D11Device* device = m_renderer->GetDevice();
            std::unordered_set<std::string> seen;
            for (const auto& subMesh : source.subMeshes)
                for (const auto& raw : { subMesh.material.albedoPath, subMesh.material.normalPath,
                                         subMesh.material.roughnessPath, subMesh.material.emissivePath })
                    if (!raw.empty() && seen.insert(raw).second)
                        LoadTextureNow(*m_assets, device, MaterialTexturePath(source.directory, raw));
            auto mesh = std::make_shared<Mesh>();
            if (!mesh->Upload(device, std::move(source)))
            {
                SE_LOG_WARN("Actor: mesh '%s' upload failed", path.c_str());
                return false;
            }
            m_assets->CacheMesh(path, mesh);
            part.mesh = std::move(mesh);
        }
    }
    if (!part.mesh)
        return false;

    part.materials = BuildSubMeshMaterials(*m_assets, *part.mesh);
    for (auto& material : part.materials)
    {
        material.receiveShadows = part.def.receiveShadows;
        if (part.def.alphaCutoff > 0.0f && material.alphaMode == AlphaMode::Opaque &&
            material.albedo && material.albedo->HasAlpha())
        {
            material.alphaMode = AlphaMode::Cutout;
            material.alphaCutoff = part.def.alphaCutoff;
        }
    }
    part.shadowSubMeshes = BuildShadowSubMeshes(part.materials);
    return true;
}

// --- Construction ---------------------------------------------------------------

bool SceneRuntime::BuildActor(LoadedActor& actor, Scene& scene, Entity& root, PhysicsWorld& physics,
                              bool loadMissing)
{
    const char* name = actor.def.name.c_str();
    actor.entity = &root;
    actor.transform = root.GetComponent<TransformComponent>();
    if (!actor.transform)
        actor.transform = root.AddComponent<TransformComponent>();
    root.AddComponent<ActorComponent>(actor.def.actorClass, actor.def.properties);
    if (!actor.def.scripts.empty())
        root.AddComponent<ScriptComponent>(actor.def.scripts);
    actor.parts.clear();
    actor.dynamicPart = -1;

    ID3D11Device* device = m_renderer->GetDevice();
    for (const ActorComponentDefinition& def : actor.def.resolved)
    {
        ActorPart part;
        part.def = def;
        Entity* entity = scene.CreateEntity(actor.def.name + "." + def.name);
        part.transform = entity->AddComponent<TransformComponent>();
        part.transform->position = { def.transform.position[0], def.transform.position[1], def.transform.position[2] };
        part.transform->eulerDeg = { def.transform.rotationDegrees[0], def.transform.rotationDegrees[1],
                                     def.transform.rotationDegrees[2] };
        part.transform->scale = { def.transform.scale[0], def.transform.scale[1], def.transform.scale[2] };
        part.transform->SetParent(actor.transform);

        switch (def.type)
        {
        case ActorComponentType::Mesh:
            if (!LoadActorMesh(part, loadMissing))
                SE_LOG_WARN("Actor '%s': mesh '%s' is not loaded; component '%s' renders nothing",
                            name, def.mesh.c_str(), def.name.c_str());
            break;
        case ActorComponentType::PointLight:
            part.light.color = { def.color[0] * def.intensity, def.color[1] * def.intensity,
                                 def.color[2] * def.intensity };
            part.light.radius = def.radius;
            break;
        case ActorComponentType::ParticleEmitter:
        {
            auto particles = std::make_unique<ParticleSystem>();
            ApplyParticleDescToSystem(def.emitter, *particles);
            if (!particles->Init(device, *m_shaders))
            {
                SE_LOG_WARN("Actor '%s': particle emitter '%s' could not be created", name, def.name.c_str());
                break;
            }
            const std::string texture = LevelDependencyPlan::NormalizePath(def.emitter.texture);
            if (!texture.empty())
                particles->SetTexture(loadMissing ? LoadTextureNow(*m_assets, device, texture)
                                                  : m_assets->FindTexture(Wide(texture)));
            part.particles = std::move(particles);
            break;
        }
        case ActorComponentType::Box:
        case ActorComponentType::Sphere:
            if (def.body == ActorBodyType::Dynamic)
            {
                if (actor.dynamicPart < 0)
                    actor.dynamicPart = static_cast<int>(actor.parts.size());
                else
                    SE_LOG_WARN("Actor '%s': component '%s' is a second dynamic body; an actor has at most "
                                "one, so it is ignored", name, def.name.c_str());
            }
            break;
        }
        actor.parts.push_back(std::move(part));
    }

    if (actor.dynamicPart >= 0)
    {
        const ActorPart& body = actor.parts[actor.dynamicPart];
        for (size_t i = 0; i < actor.parts.size(); ++i)
        {
            const ActorPart& part = actor.parts[i];
            if (static_cast<int>(i) != actor.dynamicPart &&
                (part.def.IsCollider() || part.def.collision))
                SE_LOG_WARN("Actor '%s': collider '%s' is ignored: the dynamic body '%s' is the actor's "
                            "only collision", name, part.def.name.c_str(), body.def.name.c_str());
        }
        if (actor.transform->parent)
        {
            SE_LOG_WARN("Actor '%s': a dynamic actor moves in world space, so it is detached from its parent",
                        name);
            XMFLOAT3 position, scale;
            XMFLOAT4 rotation;
            WorldPose(*actor.transform, position, rotation, scale);
            actor.transform->Unparent();
            actor.transform->position = position;
        }
        auto* rigidBody = root.AddComponent<RigidBodyComponent>();
        rigidBody->mass = body.def.mass;
        rigidBody->friction = body.def.friction;
        rigidBody->restitution = body.def.restitution;
        rigidBody->useGravity = gravityEnabled;
    }

    if (IsActive(actor.transform))
        for (ActorPart& part : actor.parts)
            if (IsActive(part.transform))
                CreatePartBody(actor, part, physics);
    return true;
}

void SceneRuntime::CreatePartBody(LoadedActor& actor, ActorPart& part, PhysicsWorld& physics)
{
    if (part.body != k_invalidBody)
        return;
    const int index = static_cast<int>(&part - actor.parts.data());
    const ActorComponentDefinition& def = part.def;

    if (actor.dynamicPart >= 0)
    {
        if (index != actor.dynamicPart)
            return;
        auto* rigidBody = actor.entity->GetComponent<RigidBodyComponent>();
        const XMFLOAT3 rootScale = actor.transform->scale;
        if (def.type == ActorComponentType::Sphere)
        {
            const float scale = (std::max)({ std::abs(rootScale.x), std::abs(rootScale.y), std::abs(rootScale.z) });
            part.body = physics.AddSphere(actor.transform, rigidBody, def.radius * scale);
            return;
        }
        // Box: a convex hull of its corners in the actor's (unscaled) body frame.
        PhysicsWorld::ConvexBodyDesc desc;
        const XMMATRIX local = part.transform->GetLocalMatrix();
        const XMFLOAT3 half = { def.size[0] * 0.5f, def.size[1] * 0.5f, def.size[2] * 0.5f };
        for (int corner = 0; corner < 8; ++corner)
        {
            const XMVECTOR point = XMVector3TransformCoord(
                XMVectorSet(corner & 1 ? half.x : -half.x, corner & 2 ? half.y : -half.y,
                            corner & 4 ? half.z : -half.z, 1.0f), local);
            XMFLOAT3 p;
            XMStoreFloat3(&p, XMVectorMultiply(point, XMLoadFloat3(&rootScale)));
            desc.hullPoints.push_back(p);
        }
        XMStoreFloat3(&desc.centerOfMass, XMVectorMultiply(local.r[3], XMLoadFloat3(&rootScale)));
        const float x = def.size[0] * part.transform->scale.x * rootScale.x;
        const float y = def.size[1] * part.transform->scale.y * rootScale.y;
        const float z = def.size[2] * part.transform->scale.z * rootScale.z;
        const float m = def.mass / 12.0f;
        desc.inertia = { m * (y * y + z * z), m * (x * x + z * z), m * (x * x + y * y) };
        part.body = physics.AddConvexBody(actor.transform, rigidBody, desc);
        return;
    }

    PhysicsWorld::KinematicShape shape;
    if (def.type == ActorComponentType::Box)
        shape.type = PhysicsWorld::KinematicShape::Type::Box;
    else if (def.type == ActorComponentType::Sphere)
        shape.type = PhysicsWorld::KinematicShape::Type::Sphere;
    else if (def.type == ActorComponentType::Mesh && def.collision && part.collider)
        shape.type = PhysicsWorld::KinematicShape::Type::Mesh;
    else
        return;

    XMFLOAT3 position, scale;
    XMFLOAT4 rotation;
    WorldPose(*part.transform, position, rotation, scale);
    shape.halfExtents = { def.size[0] * 0.5f * scale.x, def.size[1] * 0.5f * scale.y,
                          def.size[2] * 0.5f * scale.z };
    shape.radius = def.radius * (std::max)({ scale.x, scale.y, scale.z });
    shape.mesh = part.collider;
    shape.scale = scale;
    part.body = physics.AddKinematicBody(shape, position, rotation, def.friction, def.restitution,
                                         actor.transform);
    part.bodyPosition = position;
    part.bodyRotation = rotation;
}

SceneRuntime::LoadedActor* SceneRuntime::SpawnActor(SceneDescriptor::ActorEntry def,
                                                    const ResolvedActorClass& cls)
{
    if (!HasScene())
        return nullptr;
    if (!cls.error.empty())
    {
        SE_LOG_WARN("Actor '%s' (%s): %s", def.name.c_str(), def.actorClass.c_str(), cls.error.c_str());
        return nullptr;
    }
    ActorResolveResult resolved = ResolveActorComponents(cls.components, def.components);
    for (const std::string& warning : resolved.warnings)
        SE_LOG_WARN("Actor '%s' (%s): %s", def.name.c_str(), def.actorClass.c_str(), warning.c_str());
    if (!resolved.Succeeded())
    {
        SE_LOG_WARN("Actor '%s' (%s): %s", def.name.c_str(), def.actorClass.c_str(), resolved.error.c_str());
        return nullptr;
    }
    def.resolved = std::move(resolved.components);

    Entity* root = world.GetScene().CreateEntity(def.name);
    root->AddComponent<LevelIdentityComponent>(0);
    auto* transform = root->AddComponent<TransformComponent>();
    transform->position = { def.position[0], def.position[1], def.position[2] };
    transform->eulerDeg = { def.rotation[0], def.rotation[1], def.rotation[2] };
    transform->scale = { def.scale[0], def.scale[1], def.scale[2] };

    LoadedActor actor;
    actor.def = std::move(def);
    actor.spawned = true;
    BuildActor(actor, world.GetScene(), *root, physicsWorld, true);
    actors.push_back(std::move(actor));
    return &actors.back();
}

void SceneRuntime::DestroyActor(EntityID root)
{
    const auto it = std::find_if(actors.begin(), actors.end(),
                                 [root](const LoadedActor& actor) { return actor.entity && actor.entity->GetID() == root; });
    if (it == actors.end())
        return;
    for (ActorPart& part : it->parts)
    {
        physicsWorld.RemoveBody(part.body);
        if (Entity* entity = part.transform ? part.transform->GetOwner() : nullptr)
            world.DestroyEntity(*entity);
    }
    world.DestroyEntity(*it->entity);
    actors.erase(it);
}

SceneRuntime::LoadedActor* SceneRuntime::FindActor(EntityID root)
{
    for (LoadedActor& actor : actors)
        if (actor.entity && actor.entity->GetID() == root)
            return &actor;
    return nullptr;
}

const SceneRuntime::LoadedActor* SceneRuntime::FindActor(EntityID root) const
{
    for (const LoadedActor& actor : actors)
        if (actor.entity && actor.entity->GetID() == root)
            return &actor;
    return nullptr;
}

SceneRuntime::ActorPart* SceneRuntime::FindActorPart(EntityID partEntity, LoadedActor** owner)
{
    for (LoadedActor& actor : actors)
        for (ActorPart& part : actor.parts)
            if (part.transform && part.transform->GetOwner() && part.transform->GetOwner()->GetID() == partEntity)
            {
                if (owner)
                    *owner = &actor;
                return &part;
            }
    return nullptr;
}

// --- Editor placement -------------------------------------------------------------

SceneRuntime::LoadedActor SceneRuntime::CreatePlacedActor(SceneDescriptor::ActorEntry def)
{
    def.resolved.clear();
    def.resolveError.clear();
    const ResolvedActorClass cls = actorClassResolver
        ? actorClassResolver(def.actorClass)
        : ResolvedActorClass{ nlohmann::json::array(), "the actor class was not resolved (no script session)" };
    if (cls.error.empty())
    {
        ActorResolveResult resolved = ResolveActorComponents(cls.components, def.components);
        for (const std::string& warning : resolved.warnings)
            SE_LOG_WARN("Actor '%s' (%s): %s", def.name.c_str(), def.actorClass.c_str(), warning.c_str());
        if (resolved.Succeeded())
            def.resolved = std::move(resolved.components);
        else
            def.resolveError = resolved.error;
    }
    else
        def.resolveError = cls.error;
    if (!def.resolveError.empty())
        SE_LOG_WARN("Actor '%s' (%s): %s; the actor has no components", def.name.c_str(),
                    def.actorClass.c_str(), def.resolveError.c_str());

    Entity* root = world.GetScene().CreateEntity(def.name);
    root->AddComponent<LevelIdentityComponent>(def.sourceEntityID);
    auto* transform = root->AddComponent<TransformComponent>();
    transform->position = { def.position[0], def.position[1], def.position[2] };
    transform->eulerDeg = { def.rotation[0], def.rotation[1], def.rotation[2] };
    transform->scale = { def.scale[0], def.scale[1], def.scale[2] };

    LoadedActor actor;
    actor.def = std::move(def);
    BuildActor(actor, world.GetScene(), *root, physicsWorld, true);
    return actor;
}

int SceneRuntime::AddActor(const std::string& classPath, const XMFLOAT3& position)
{
    if (!HasScene())
        return -1;
    SceneDescriptor::ActorEntry def;
    def.actorClass = classPath;
    def.name = std::filesystem::path(classPath).stem().string();
    def.position = { position.x, position.y, position.z };
    actors.push_back(CreatePlacedActor(std::move(def)));
    return static_cast<int>(actors.size()) - 1;
}

SceneDescriptor::ActorEntry SceneRuntime::ActorPlacement(int index) const
{
    if (index < 0 || index >= static_cast<int>(actors.size()))
        return {};
    const LoadedActor& actor = actors[index];
    SceneDescriptor::ActorEntry def = actor.def;
    if (const TransformComponent* t = actor.transform)
    {
        def.position = { t->position.x, t->position.y, t->position.z };
        def.rotation = { t->eulerDeg.x, t->eulerDeg.y, t->eulerDeg.z };
        def.scale = { t->scale.x, t->scale.y, t->scale.z };
    }
    return def;
}

SceneDescriptor::ActorEntry SceneRuntime::RemoveActor(int index)
{
    if (index < 0 || index >= static_cast<int>(actors.size()))
        return {};
    SceneDescriptor::ActorEntry def = ActorPlacement(index);
    DestroyActor(actors[index].entity->GetID());
    return def;
}

void SceneRuntime::InsertActor(int index, SceneDescriptor::ActorEntry def)
{
    if (!HasScene())
        return;
    index = std::clamp(index, 0, static_cast<int>(actors.size()));
    actors.insert(actors.begin() + index, CreatePlacedActor(std::move(def)));
}

int SceneRuntime::DuplicateActor(int index)
{
    if (index < 0 || index >= static_cast<int>(actors.size()))
        return -1;
    SceneDescriptor::ActorEntry def = ActorPlacement(index);
    def.sourceEntityID = 0; // a fresh id is assigned on save
    actors.push_back(CreatePlacedActor(std::move(def)));
    return static_cast<int>(actors.size()) - 1;
}

void SceneRuntime::RebuildActor(int index)
{
    if (index < 0 || index >= static_cast<int>(actors.size()))
        return;
    InsertActor(index, RemoveActor(index));
}

// --- Per frame --------------------------------------------------------------------

void SceneRuntime::RefreshActorState(LoadedActor& actor)
{
    const bool rootActive = IsActive(actor.transform);
    for (ActorPart& part : actor.parts)
    {
        const bool on = rootActive && IsActive(part.transform);
        if (part.particles)
            part.particles->enabled = on;
        if (on)
        {
            CreatePartBody(actor, part, physicsWorld);
        }
        else if (part.body != k_invalidBody)
        {
            physicsWorld.RemoveBody(part.body);
            part.body = k_invalidBody;
        }
    }
}

void SceneRuntime::UpdateActors()
{
    for (LoadedActor& actor : actors)
    {
        for (size_t i = 0; i < actor.parts.size(); ++i)
        {
            ActorPart& part = actor.parts[i];
            if (part.particles)
                part.particles->config.position = WorldPosition(*part.transform);
            if (part.body == k_invalidBody || static_cast<int>(i) == actor.dynamicPart)
                continue;
            XMFLOAT3 position, scale;
            XMFLOAT4 rotation;
            WorldPose(*part.transform, position, rotation, scale);
            const bool moved = position.x != part.bodyPosition.x || position.y != part.bodyPosition.y ||
                               position.z != part.bodyPosition.z || rotation.x != part.bodyRotation.x ||
                               rotation.y != part.bodyRotation.y || rotation.z != part.bodyRotation.z ||
                               rotation.w != part.bodyRotation.w;
            if (!moved)
                continue;
            physicsWorld.MoveKinematic(part.body, position, rotation);
            part.bodyPosition = position;
            part.bodyRotation = rotation;
        }
    }
}

} // namespace SE
