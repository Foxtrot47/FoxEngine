#include "Engine/Scene/LevelWorld.h"

#include <array>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include <DirectXMath.h>

#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/TransformComponent.h"

namespace SE {

namespace {

// Lights, emitters, primitives and cameras store a world-space position in
// their component; the entity transform mirrors it so scripts see the same
// position the renderer uses. A mesh owns its transform, so it wins.
std::optional<std::array<float, 3>> ComponentWorldPosition(const LevelEntityDefinition& definition)
{
    if (definition.meshRenderer)
        return std::nullopt;
    if (definition.pointLight)
        return definition.pointLight->position;
    if (definition.particleEmitter)
        return definition.particleEmitter->position;
    if (definition.primitiveRenderer)
        return definition.primitiveRenderer->position;
    if (definition.camera)
        return definition.camera->eye;
    return std::nullopt;
}

} // namespace

bool LevelWorld::Instantiate(const LevelAsset& asset)
{
    m_scene = Scene{};
    m_entitiesBySourceID.clear();
    m_primaryCamera = nullptr;

    for (const auto& definition : asset.entities)
    {
        Entity* entity = m_scene.CreateEntity(definition.name);
        entity->active = definition.active;
        entity->AddComponent<LevelIdentityComponent>(definition.id);
        auto* transform = entity->AddComponent<TransformComponent>();
        if (definition.transform)
        {
            transform->position = {
                definition.transform->position[0],
                definition.transform->position[1],
                definition.transform->position[2]
            };
            transform->eulerDeg = {
                definition.transform->rotationDegrees[0],
                definition.transform->rotationDegrees[1],
                definition.transform->rotationDegrees[2]
            };
            transform->scale = {
                definition.transform->scale[0],
                definition.transform->scale[1],
                definition.transform->scale[2]
            };
        }
        if (const auto worldPosition = ComponentWorldPosition(definition))
            transform->position = { (*worldPosition)[0], (*worldPosition)[1], (*worldPosition)[2] };

        if (definition.meshRenderer)
            entity->AddComponent<MeshRendererComponent>(*definition.meshRenderer);
        if (definition.directionalLight)
            entity->AddComponent<DirectionalLightComponent>(*definition.directionalLight);
        if (definition.pointLight)
            entity->AddComponent<PointLightComponent>(*definition.pointLight);
        if (definition.primitiveRenderer)
            entity->AddComponent<PrimitiveRendererComponent>(*definition.primitiveRenderer);
        if (definition.particleEmitter)
            entity->AddComponent<ParticleEmitterComponent>(*definition.particleEmitter);
        if (definition.camera)
        {
            auto* camera = entity->AddComponent<CameraComponent>();
            camera->eye = {
                definition.camera->eye[0], definition.camera->eye[1], definition.camera->eye[2]
            };
            camera->nearZ = definition.camera->nearZ;
            camera->farZ = definition.camera->farZ;
            if (definition.active && (!m_primaryCamera || definition.camera->primary))
                m_primaryCamera = camera;
        }
        if (!definition.scripts.empty())
            entity->AddComponent<ScriptComponent>(definition.scripts);

        m_entitiesBySourceID.emplace(definition.id, entity);
    }

    for (const auto& definition : asset.entities)
    {
        if (definition.parent == 0)
            continue;
        Entity* child = FindBySourceID(definition.id);
        Entity* parent = FindBySourceID(definition.parent);
        if (child && parent)
            child->GetComponent<TransformComponent>()->SetParent(
                parent->GetComponent<TransformComponent>());
    }

    // Component positions are world space: once the hierarchy exists, move
    // them into the parent's space. Shallower entities convert first so every
    // parent world matrix used below is already final.
    std::vector<std::pair<size_t, TransformComponent*>> converts;
    for (const auto& definition : asset.entities)
    {
        if (definition.parent == 0 || !ComponentWorldPosition(definition))
            continue;
        Entity* child = FindBySourceID(definition.id);
        auto* transform = child ? child->GetComponent<TransformComponent>() : nullptr;
        if (!transform || !transform->parent)
            continue;
        size_t depth = 0;
        for (const TransformComponent* ancestor = transform->parent; ancestor; ancestor = ancestor->parent)
            ++depth;
        converts.emplace_back(depth, transform);
    }
    std::stable_sort(converts.begin(), converts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& convert : converts)
    {
        using namespace DirectX;
        TransformComponent* transform = convert.second;
        XMVECTOR determinant;
        const XMMATRIX parentInverse =
            XMMatrixInverse(&determinant, transform->parent->GetWorldMatrix());
        XMStoreFloat3(&transform->position,
                      XMVector3TransformCoord(XMLoadFloat3(&transform->position), parentInverse));
    }

    return true;
}

Entity* LevelWorld::FindBySourceID(LevelEntityID id) const
{
    const auto found = m_entitiesBySourceID.find(id);
    return found == m_entitiesBySourceID.end() ? nullptr : found->second;
}

} // namespace SE
