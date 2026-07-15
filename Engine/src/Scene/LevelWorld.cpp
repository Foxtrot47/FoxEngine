#include "Engine/Scene/LevelWorld.h"

#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/TransformComponent.h"

namespace SE {

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

    return true;
}

Entity* LevelWorld::FindBySourceID(LevelEntityID id) const
{
    const auto found = m_entitiesBySourceID.find(id);
    return found == m_entitiesBySourceID.end() ? nullptr : found->second;
}

} // namespace SE
