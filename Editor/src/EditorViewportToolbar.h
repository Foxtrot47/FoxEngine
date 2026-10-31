#pragma once
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

// Draw before the viewport image: toolbar controls never overlap picking.
void DrawViewportToolbar(EditorCamera& camera, SE::CameraComponent& component,
                         GizmoOp& operation, GizmoSettings& gizmo, ViewportOptions& options,
                         SE::ForwardPipeline& pipeline);
void DrawOrthographicGrid(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max);
void DrawOrientationAxes(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max);
const char* ViewModeLabel(EditorCamera::ViewMode mode);
} // namespace Editor
