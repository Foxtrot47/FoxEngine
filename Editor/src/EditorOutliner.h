#pragma once
#include <DirectXMath.h>
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

// Scene tree: camera, lights, meshes, JSON objects, and particle emitters.
// Clicking a row updates the selection; the Add button spawns entities at
// spawnPos and rows offer right-click Delete. Returns true when an entity
// was deleted (index-based undo entries must be discarded).
bool DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel,
                       const DirectX::XMFLOAT3& spawnPos);

// Removes the selected entity when its kind is deletable (point light,
// mesh, object, emitter) and clears the selection. Returns true on delete.
bool DeleteSelection(SE::SceneRuntime& scene, Selection& sel);

// Appends a copy of the selected entity and moves the selection to it.
bool DuplicateSelection(SE::SceneRuntime& scene, Selection& sel);

// Starts inline outliner rename for the selection when its name is editable.
void RequestRenameSelection(SE::SceneRuntime& scene, const Selection& sel);

} // namespace Editor
