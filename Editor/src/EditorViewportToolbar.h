#pragma once
#include <cstdint>
#include "EditorCamera.h"
#include "EditorGizmo.h"

namespace SE { struct CameraComponent; class ForwardPipeline; }
namespace Editor {

struct ViewportOptions
{
    bool showStats = false;
    bool showLights = true;
    bool showAxes = true;
    bool showGrid = true;
    bool gameView = false;
    bool realtime = true;
    bool immersive = false;
};

// Play/Stop state shown at the left of the toolbar.
struct PlayControls
{
    bool     playing = false;          // a play session is active
    bool     canToggle = true;         // false while a play transition loads
    uint32_t errorCount = 0;           // script errors of the play session
    const char* title = nullptr;       // game-set window title (fox.window.set_title)
    bool     toggleRequested = false;  // out: Play/Stop was clicked
};

// Draw before the viewport image: toolbar controls never overlap picking.
// While playing only the play controls and view menus are shown (the editor
// camera and transform tools are inactive).
void DrawViewportToolbar(EditorCamera& camera, SE::CameraComponent& component,
                         GizmoOp& operation, GizmoSettings& gizmo, ViewportOptions& options,
                         SE::ForwardPipeline& pipeline, PlayControls& play);
void DrawOrthographicGrid(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max);
void DrawOrientationAxes(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max);
const char* ViewModeLabel(EditorCamera::ViewMode mode);
} // namespace Editor
