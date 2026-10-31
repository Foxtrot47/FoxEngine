#pragma once
#include <DirectXMath.h>
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

class UndoStack;

// Scene tree: camera, lights, meshes, JSON objects, and particle emitters.
// Clicking a row updates the selection; the Add button spawns entities at
// spawnPos and rows offer right-click Rename / Duplicate / Delete, all
// recorded on the undo stack.
void DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                       const DirectX::XMFLOAT3& spawnPos);

// Starts inline outliner rename for the selection when its name is editable.
void RequestRenameSelection(SE::SceneRuntime& scene, const Selection& sel);

} // namespace Editor
