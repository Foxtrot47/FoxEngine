#pragma once
#include <memory>
#include "EditorSelection.h"

struct ID3D11Device;

namespace SE {
class SceneRuntime;
class ShaderLibrary;
}

namespace Editor {

class UndoStack;
struct EntityState;

// Turns Details-panel edits of the selected entity into undo entries. Begin
// snapshots the entity before its widgets draw; End watches ImGui's edit
// flag and, once the interaction finishes (drag released, combo closed,
// text committed), pushes one entry restoring the whole entity state.
// World settings are not tracked.
class PropertyUndoTracker
{
public:
    void Begin(SE::SceneRuntime& scene, const Selection& sel);
    void End(SE::SceneRuntime& scene, UndoStack& undo, ID3D11Device* device,
             SE::ShaderLibrary& shaders);

private:
    std::shared_ptr<const EntityState> m_before;
    bool m_pending       = false;
    bool m_editedBefore  = false;
};

} // namespace Editor
