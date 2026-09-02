#pragma once
#include <imgui.h>
#include "EditorSelection.h"
#include "EditorUndo.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

// Gizmo operation selected via Q/W/E/R (UE bindings).
enum class GizmoOp { None, Translate, Rotate, Scale };

// Draws an ImGuizmo manipulator for the current selection into the viewport
// image rect and applies edits (with undo capture on drag end). Returns true
// while the gizmo is hovered or actively used, which blocks click picking.
bool DrawGizmo(SE::SceneRuntime& scene, const Selection& sel, UndoStack& undo,
               GizmoOp op, ImVec2 imageMin, ImVec2 imageMax);

// Ray-picks meshes and JSON scene objects through the viewport pixel under
// mousePos. Returns a None selection when nothing is hit.
Selection PickScene(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                    ImVec2 mousePos);

} // namespace Editor
