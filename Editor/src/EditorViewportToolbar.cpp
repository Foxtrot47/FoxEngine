#include "EditorViewportToolbar.h"
#include "EditorIcons.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace Editor {
namespace {
using VM = EditorCamera::ViewMode;
struct ViewEntry { VM mode; const char* label; const char* shortcut; };
constexpr ViewEntry k_views[] = {
    { VM::Perspective, "Perspective", "Alt+G" },
    { VM::Top, "Top", "Alt+J" }, { VM::Bottom, "Bottom", "Alt+Shift+J" },
    { VM::Front, "Front", "Alt+H" }, { VM::Back, "Back", "Alt+Shift+H" },
    { VM::Left, "Left", "Alt+K" }, { VM::Right, "Right", "Alt+Shift+K" },
    { VM::Isometric, "Isometric", nullptr }, { VM::Orthographic, "Orthographic", nullptr }
};

void Hint(const char* text)
{
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ToolButton(const char* label, const char* hint, GizmoOp value, GizmoOp& current)
{
    const bool active = current == value;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    if (ImGui::Button(label)) current = value;
    if (active) ImGui::PopStyleColor();
    Hint(hint);
}

void DrawTransformTools(GizmoOp& operation, GizmoSettings& settings)
{
    ToolButton("Select", "Select (Q)", GizmoOp::None, operation); ImGui::SameLine();
    ToolButton("Move", "Translate (W)", GizmoOp::Translate, operation); ImGui::SameLine();
    ToolButton("Rotate", "Rotate (E)", GizmoOp::Rotate, operation); ImGui::SameLine();
    ToolButton("Scale", "Scale (R)\nSpace: cycle transform tools", GizmoOp::Scale, operation);
    ImGui::SameLine();
    if (ImGui::Button(settings.localSpace ? "Local" : "World")) settings.localSpace = !settings.localSpace;
    Hint("Transform coordinate system. Scale operates on object-local axes in ImGuizmo.");
}

void SnapControl(const char* label, bool& enabled, float& value, const float* values, int count)
{
    ImGui::PushID(label);
    ImGui::Checkbox(label, &enabled); ImGui::SameLine();
    char preview[32]; sprintf_s(preview, "%.3g", value);
    ImGui::SetNextItemWidth(76.0f);
    if (ImGui::BeginCombo("##step", preview))
    {
        for (int i = 0; i < count; ++i)
        {
            char text[32]; sprintf_s(text, "%.3g", values[i]);
            if (ImGui::Selectable(text, value == values[i])) value = values[i];
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void DrawSnapping(GizmoSettings& settings)
{
    if (!ImGui::BeginMenu("Snap")) return;
    static constexpr float translation[] = { 0.1f, 0.25f, 0.5f, 1, 5, 10, 50, 100 };
    static constexpr float rotation[] = { 1, 5, 10, 15, 30, 45, 90 };
    static constexpr float scale[] = { 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1 };
    SnapControl("Move", settings.translationSnap, settings.translationStep, translation, 8);
    SnapControl("Rotate", settings.rotationSnap, settings.rotationStep, rotation, 7);
    SnapControl("Scale", settings.scaleSnap, settings.scaleStep, scale, 6);
    ImGui::TextDisabled("Move: world units | Rotate: degrees | Scale: factor");
    ImGui::EndMenu();
}

void DrawCameraMenu(EditorCamera& camera, SE::CameraComponent& component)
{
    if (!ImGui::BeginMenu(ViewModeLabel(camera.GetViewMode()))) return;
    for (const auto& entry : k_views)
        if (ImGui::MenuItem(entry.label, entry.shortcut, camera.GetViewMode() == entry.mode))
            camera.SetViewMode(entry.mode, component);
    ImGui::Separator();
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("Sensitivity", &camera.sensitivity, 0.05f, 0.5f, "%.2f");
    if (component.orthographic)
    {
        ImGui::SetNextItemWidth(150);
        ImGui::DragFloat("View height", &component.orthoHeight, 0.1f, 0.05f, 100000.0f);
    }
    else
    {
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("Field of view", &component.fovDeg, 20.0f, 120.0f, "%.0f deg");
    }
    ImGui::EndMenu();
}

void DrawDisplayMenu(ViewportOptions& options)
{
    if (!ImGui::BeginMenu("Show")) return;
    ImGui::MenuItem("Light icons", nullptr, &options.showLights);
    ImGui::MenuItem("Orientation axes", nullptr, &options.showAxes);
    ImGui::MenuItem("Orthographic grid", nullptr, &options.showGrid);
    ImGui::MenuItem("Statistics", nullptr, &options.showStats);
    ImGui::Separator();
    ImGui::MenuItem("Game View", "G", &options.gameView);
    ImGui::MenuItem("Realtime", "Ctrl+R", &options.realtime);
    ImGui::MenuItem("Immersive", "F11", &options.immersive);
    ImGui::EndMenu();
}

void DrawShadingMenu(SE::ForwardPipeline& pipeline)
{
    using Shading = SE::ForwardPipeline::ViewShading;
    static const char* labels[] = { "Lit", "Unlit", "Wireframe" };
    if (!ImGui::BeginMenu(labels[static_cast<int>(pipeline.viewShading)])) return;
    for (int i = 0; i < 3; ++i)
        if (ImGui::MenuItem(labels[i], nullptr, static_cast<int>(pipeline.viewShading) == i))
            pipeline.viewShading = static_cast<Shading>(i);
    ImGui::Separator();
    ImGui::TextDisabled("Viewport only; scene materials are unchanged.");
    ImGui::TextDisabled("Orthographic: perspective-only fog, SSAO and FSR omitted.");
    ImGui::EndMenu();
}

void DrawNavigationHelp()
{
    if (!ImGui::BeginMenu("?")) return;
    ImGui::TextUnformatted("Perspective navigation");
    ImGui::TextDisabled("RMB + WASD / QE: fly | RMB + wheel: speed");
    ImGui::TextDisabled("LMB drag: forward/back + turn | LMB+RMB: pan");
    ImGui::TextUnformatted("Orthographic navigation");
    ImGui::TextDisabled("RMB / MMB: pan | LMB+RMB / wheel: zoom");
    ImGui::TextUnformatted("Both projections");
    ImGui::TextDisabled("Alt+LMB: orbit | Alt+RMB: dolly | Alt+MMB: track");
    ImGui::TextDisabled("F: frame selection | Q/W/E/R: transform tool");
    ImGui::TextDisabled("View changes preserve position and orthographic zoom.");
    ImGui::EndMenu();
}
} // namespace

const char* ViewModeLabel(VM mode)
{
    for (const auto& entry : k_views)
        if (entry.mode == mode) return entry.label;
    return "Perspective";
}

namespace {

void DrawPlayControls(PlayControls& play)
{
    const ImVec4 tint = play.playing ? ImVec4(0.75f, 0.22f, 0.22f, 1.0f)
                                     : ImVec4(0.20f, 0.55f, 0.25f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, tint);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(tint.x * 1.25f, tint.y * 1.25f, tint.z * 1.25f, 1.0f));
    ImGui::BeginDisabled(!play.canToggle);
    if (ImGui::Button(play.playing ? ICON_MD_STOP " Stop" : ICON_MD_PLAY_ARROW " Play"))
        play.toggleRequested = true;
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(play.playing
            ? "Stop (F5) and restore the edited scene\nShift+F1 releases game focus"
            : "Play (F5) the current edit state with the project's scripts");
    if (!play.playing)
        return;
    ImGui::SameLine();
    if (play.errorCount > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), ICON_MD_ERROR " %u", play.errorCount);
    else
        ImGui::TextDisabled(ICON_MD_ERROR " 0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Script errors this session (details in the Console)");
    if (play.title && play.title[0] != '\0')
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", play.title);
    }
}

} // namespace

void DrawViewportToolbar(EditorCamera& camera, SE::CameraComponent& component,
                         GizmoOp& operation, GizmoSettings& gizmo, ViewportOptions& options,
                         SE::ForwardPipeline& pipeline, PlayControls& play)
{
    // Menus retain all commands when the docked viewport is too narrow for
    // the inline transform group. No dead or non-functional toolbar buttons.
    if (!ImGui::BeginMenuBar()) return;
    DrawPlayControls(play);
    ImGui::SameLine();
    if (!play.playing)
    {
        if (ImGui::GetWindowWidth() > 920.0f)
            DrawTransformTools(operation, gizmo);
        else if (ImGui::BeginMenu("Transform"))
        {
            DrawTransformTools(operation, gizmo);
            ImGui::EndMenu();
        }
        DrawSnapping(gizmo);
        DrawCameraMenu(camera, component);
        if (ImGui::BeginMenu("Speed"))
        {
            ImGui::SliderFloat("Units / second", &camera.moveSpeed, 0.2f, 400.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled("RMB + wheel adjusts speed; Shift boosts it.");
            ImGui::EndMenu();
        }
    }
    DrawShadingMenu(pipeline);
    DrawDisplayMenu(options);
    DrawNavigationHelp();
    ImGui::EndMenuBar();
}

namespace {
void DrawGridLines(float start, float extent, float spacing, float pixelsPerUnit,
                   bool vertical, ImVec2 min, ImVec2 max)
{
    const float first = std::ceil(start / spacing);
    const int count = (std::min)(256, static_cast<int>(std::ceil(extent / spacing)) + 1);
    auto* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i)
    {
        const float coordinate = (first + static_cast<float>(i)) * spacing;
        const float offset = (coordinate - start) * pixelsPerUnit;
        const bool major = std::fabs(std::remainder(first + static_cast<float>(i), 10.0f)) < 0.1f;
        const ImU32 color = major ? IM_COL32(190, 190, 190, 55) : IM_COL32(190, 190, 190, 22);
        if (vertical) draw->AddLine({ min.x + offset, min.y }, { min.x + offset, max.y }, color);
        else draw->AddLine({ min.x, max.y - offset }, { max.x, max.y - offset }, color);
    }
}
}

void DrawOrthographicGrid(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max)
{
    using namespace DirectX;
    if (!camera.orthographic || max.y <= min.y) return;
    const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&camera.target) - XMLoadFloat3(&camera.eye));
    const XMVECTOR absolute = XMVectorAbs(forward);
    const float alignment = (std::max)({ XMVectorGetX(absolute), XMVectorGetY(absolute), XMVectorGetZ(absolute) });
    if (alignment < 0.9999f) return; // the drafting grid belongs to axis-aligned views
    const XMVECTOR up = XMLoadFloat3(&camera.up);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    const float height = camera.orthoHeight;
    const float width = height * (max.x - min.x) / (max.y - min.y);
    const float scale = (max.y - min.y) / height;
    const float spacing = powf(10.0f, floorf(log10f(50.0f / scale)));
    const float left = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&camera.eye), right)) - width * 0.5f;
    const float bottom = XMVectorGetX(XMVector3Dot(XMLoadFloat3(&camera.eye), up)) - height * 0.5f;
    ImGui::GetWindowDrawList()->PushClipRect(min, max, true);
    DrawGridLines(left, width, spacing, scale, true, min, max);
    DrawGridLines(bottom, height, spacing, scale, false, min, max);
    ImGui::GetWindowDrawList()->PopClipRect();
}

void DrawOrientationAxes(const SE::CameraComponent& camera, ImVec2 min, ImVec2 max)
{
    using namespace DirectX;
    if (max.x - min.x < 100 || max.y - min.y < 120) return;
    const ImVec2 center = { min.x + 44.0f, max.y - 55.0f };
    struct Axis { XMVECTOR direction; const char* label; ImU32 color; float depth; };
    Axis axes[] = {
        { XMVectorSet(1, 0, 0, 0), "X", IM_COL32(232, 85, 85, 255), 0 },
        { XMVectorSet(0, 1, 0, 0), "Y", IM_COL32(105, 201, 107, 255), 0 },
        { XMVectorSet(0, 0, 1, 0), "Z", IM_COL32(85, 150, 240, 255), 0 }
    };
    for (auto& axis : axes)
    {
        axis.direction = XMVector3TransformNormal(axis.direction, camera.GetViewMatrix());
        axis.depth = XMVectorGetZ(axis.direction);
    }
    std::sort(std::begin(axes), std::end(axes), [](const Axis& a, const Axis& b) { return a.depth > b.depth; });
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(center, 34.0f, IM_COL32(12, 12, 12, 155));
    for (const auto& axis : axes)
    {
        const ImVec2 tip = { center.x + XMVectorGetX(axis.direction) * 25.0f,
                            center.y - XMVectorGetY(axis.direction) * 25.0f };
        draw->AddLine(center, tip, axis.color, 2.0f);
        draw->AddText({ tip.x + 2, tip.y - 8 }, axis.color, axis.label);
    }
}
} // namespace Editor
