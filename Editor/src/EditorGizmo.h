#pragma once
#include <DirectXMath.h>
#include <imgui.h>
#include "EditorSelection.h"
#include "EditorUndo.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

// Gizmo operation selected via Q/W/E/R (UE bindings).
enum class GizmoOp { None, Translate, Rotate, Scale };

struct GizmoSettings
{
    bool localSpace = false;
    bool translationSnap = false;
    bool rotationSnap = false;
    bool scaleSnap = false;
    float translationStep = 1.0f; // engine world units, not Unreal centimetres
    float rotationStep = 15.0f;   // degrees
    float scaleStep = 0.1f;       // additive scale factor
};

// World-space TRS of an editable entity; point targets (lights, emitters,
// objects) use position only.
struct TransformSnapshot
{
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    DirectX::XMFLOAT3 euler    = { 0, 0, 0 };
    DirectX::XMFLOAT3 scale    = { 1, 1, 1 };
};

// Meshes, empties and actors carry rotation and scale.
bool HasFullTransform(Selection::Kind kind);
// False for selections without a position (None, World, Camera, Sun, stale index).
bool CaptureTransform(SE::SceneRuntime& scene, const Selection& sel, TransformSnapshot& out);
// Writes the runtime and level-definition transform (mesh colliders keep their load pose).
void ApplyTransform(SE::SceneRuntime& scene, const Selection& sel, const TransformSnapshot& s);
// Pushes a "Transform" undo entry unless before == after.
void PushTransformUndo(SE::SceneRuntime& scene, UndoStack& undo, Selection sel,
                       const TransformSnapshot& before, const TransformSnapshot& after);

// Draws an ImGuizmo manipulator for the current selection into the viewport
// image rect and applies edits (with undo capture on drag end). Returns true
// while the gizmo is hovered or actively used, which blocks click picking.
bool DrawGizmo(SE::SceneRuntime& scene, const Selection& sel, UndoStack& undo,
               GizmoOp op, const GizmoSettings& settings,
               ImVec2 imageMin, ImVec2 imageMax);

// Ray-picks meshes and JSON scene objects through the viewport pixel under
// mousePos. Returns a None selection when nothing is hit.
Selection PickScene(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                    ImVec2 mousePos);

// World-space framing sphere of the selection (for F focus). Returns false
// for selections without a focusable position (None, Camera, Sun).
bool GetSelectionBounds(SE::SceneRuntime& scene, const Selection& sel,
                        DirectX::XMFLOAT3& outCenter, float& outRadius);

} // namespace Editor
