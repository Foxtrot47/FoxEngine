#pragma once

#include <unordered_map>
#include "Engine/Scene/LevelAsset.h"
#include "Engine/Scene/Scene.h"

namespace SE {

struct CameraComponent;

class LevelWorld
{
public:
    bool Instantiate(const LevelAsset& asset);

    Scene& GetScene() { return m_scene; }
    const Scene& GetScene() const { return m_scene; }
    Entity* FindBySourceID(LevelEntityID id) const;
    // Destroys the entity: detaches it from its parent and its children (who
    // keep their local transform as a top-level one) and forgets its source id.
    void DestroyEntity(Entity& entity);
    CameraComponent* GetPrimaryCamera() const { return m_primaryCamera; }

private:
    Scene m_scene;
    std::unordered_map<LevelEntityID, Entity*> m_entitiesBySourceID;
    CameraComponent* m_primaryCamera = nullptr;
};

} // namespace SE
