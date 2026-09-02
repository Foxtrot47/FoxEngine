#pragma once
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

// Scene tree: camera, lights, meshes, JSON objects, and particle emitters.
// Clicking a row updates the selection.
void DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel);

} // namespace Editor
