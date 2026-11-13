#pragma once
#include <DirectXMath.h>
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

class UndoStack;
class ScriptCatalog;

// Scene tree: camera, lights, meshes, JSON objects, particle emitters,
// empties and actors. Clicking a row updates the selection; the Add button
// spawns entities (and actors of the project's fox.actor classes) at
// spawnPos and rows offer right-click Rename / Duplicate / Delete, all
// recorded on the undo stack. While playing rows stay selectable but
// nothing can be edited.
void DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                       const DirectX::XMFLOAT3& spawnPos, ScriptCatalog& scripts, bool playing);

// Starts inline outliner rename for the selection when its name is editable.
void RequestRenameSelection(SE::SceneRuntime& scene, const Selection& sel);

} // namespace Editor
