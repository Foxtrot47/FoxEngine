#include "EditorOutliner.h"
#include "EditorIcons.h"

#include "Engine/Scene/SceneRuntime.h"
#include <imgui.h>
#include <cstdio>

namespace Editor {

namespace {

struct IconDesc
{
    const char* glyph;
    ImU32       color;
};

// Icon glyph centered inside a line-height cell (vertical centering is done
// here instead of relying on the merged font's baseline).
void DrawIconCentered(const IconDesc& icon, float cellW, float cellH)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ cellW, cellH });
    const ImVec2 sz = ImGui::CalcTextSize(icon.glyph);
    ImGui::GetWindowDrawList()->AddText(
        { pos.x + (cellW - sz.x) * 0.5f, pos.y + (cellH - sz.y) * 0.5f },
        icon.color, icon.glyph);
}

// Small clickable eye toggling *visible; pass nullptr for entities that
// cannot be hidden (camera, level root).
void DrawEyeToggle(bool* visible)
{
    const float h = ImGui::GetTextLineHeight() + 4.0f;
    if (!visible)
    {
        ImGui::Dummy({ h, h });
        return;
    }
    ImGui::InvisibleButton("##eye", { h, h });
    if (ImGui::IsItemClicked())
        *visible = !*visible;

    const bool  hovered = ImGui::IsItemHovered();
    const ImU32 col = *visible ? IM_COL32(210, 210, 210, hovered ? 255 : 220)
                               : IM_COL32(130, 130, 130, hovered ? 255 : 170);
    const char* glyph = *visible ? ICON_MD_VISIBILITY : ICON_MD_VISIBILITY_OFF;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 sz = ImGui::CalcTextSize(glyph);
    ImGui::GetWindowDrawList()->AddText(
        { mn.x + (h - sz.x) * 0.5f, mn.y + (h - sz.y) * 0.5f }, col, glyph);
}

// Returns true when the row's context menu requested a delete. Deletable
// rows also select themselves on right-click, UE-style.
bool DrawRow(Selection& sel, Selection::Kind kind, int index, const char* label,
             const char* typeName, const IconDesc& icon, bool* visible, bool indent,
             bool deletable = false)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    ImGui::PushID(index);

    if (indent)
        ImGui::Indent(14.0f);
    const float rowH = ImGui::GetTextLineHeight() + 4.0f;
    if (ImGui::Selectable("##row", sel.Is(kind, index),
                          ImGuiSelectableFlags_SpanAllColumns |
                          ImGuiSelectableFlags_AllowOverlap, { 0.0f, rowH }))
        sel.Set(kind, index);
    bool wantDelete = false;
    if (deletable && ImGui::BeginPopupContextItem("##ctx"))
    {
        sel.Set(kind, index);
        if (ImGui::MenuItem("Delete", "Del"))
            wantDelete = true;
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 0.0f);
    DrawIconCentered(icon, rowH, rowH);
    ImGui::SameLine(0.0f, 5.0f);
    const ImVec2 textPos = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(
        { textPos.x, textPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f });
    ImGui::TextUnformatted(label);
    if (indent)
        ImGui::Unindent(14.0f);

    ImGui::TableSetColumnIndex(0);
    DrawEyeToggle(visible);

    ImGui::TableSetColumnIndex(2);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextDisabled("%s", typeName);

    ImGui::PopID();
    ImGui::PopID();
    return wantDelete;
}

// Menu item with a colored icon glyph in front of the label.
bool IconMenuItem(const IconDesc& icon, const char* label, bool enabled = true)
{
    char padded[64];
    sprintf_s(padded, "       %s", label); // room for the glyph column
    const bool clicked = ImGui::MenuItem(padded, nullptr, false, enabled);
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 sz = ImGui::CalcTextSize(icon.glyph);
    const ImU32 col = enabled ? icon.color
                              : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddText(
        { mn.x + 6.0f, mn.y + (ImGui::GetItemRectSize().y - sz.y) * 0.5f },
        col, icon.glyph);
    return clicked;
}

// "+ Add" toolbar button: spawns a new entity at spawnPos and selects it.
void DrawAddMenu(SE::SceneRuntime& scene, Selection& sel,
                 const DirectX::XMFLOAT3& spawnPos)
{
    using Kind = Selection::Kind;
    using ObjType = SE::SceneDescriptor::SceneObject::Type;
    if (ImGui::Button(ICON_MD_ADD " Add"))
        ImGui::OpenPopup("##add_entity");
    if (!ImGui::BeginPopup("##add_entity"))
        return;
    ImGui::SeparatorText("Shapes");
    if (IconMenuItem({ ICON_MD_PANORAMA_FISH_EYE, IM_COL32(150, 210, 220, 255) },
                     "Sphere") &&
        scene.AddSceneObject(ObjType::Sphere, spawnPos))
        sel.Set(Kind::Object, (int)scene.sceneObjects.size() - 1);
    if (IconMenuItem({ ICON_MD_CROP_LANDSCAPE, IM_COL32(150, 210, 220, 255) },
                     "Plane") &&
        scene.AddSceneObject(ObjType::Plane, spawnPos))
        sel.Set(Kind::Object, (int)scene.sceneObjects.size() - 1);
    ImGui::SeparatorText("Lights");
    if (IconMenuItem({ ICON_MD_LIGHTBULB_OUTLINE, IM_COL32(255, 170, 80, 255) },
                     "Point Light", scene.lights.numLights < 8) &&
        scene.AddPointLight(spawnPos))
        sel.Set(Kind::PointLight, scene.lights.numLights - 1);
    ImGui::SeparatorText("Effects");
    if (IconMenuItem({ ICON_MD_GRAIN, IM_COL32(210, 150, 255, 255) },
                     "Particle Emitter") &&
        scene.AddParticleEmitter(spawnPos))
        sel.Set(Kind::Emitter, (int)scene.particleSystems.size() - 1);
    ImGui::EndPopup();
}

} // namespace

bool DeleteSelection(SE::SceneRuntime& scene, Selection& sel)
{
    using Kind = Selection::Kind;
    switch (sel.kind)
    {
    case Kind::PointLight: scene.RemovePointLight(sel.index); break;
    case Kind::Mesh:       scene.RemoveMesh(sel.index); break;
    case Kind::Object:     scene.RemoveSceneObject(sel.index); break;
    case Kind::Emitter:    scene.RemoveParticleEmitter(sel.index); break;
    default: return false;
    }
    sel.Clear();
    return true;
}

bool DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel,
                       const DirectX::XMFLOAT3& spawnPos)
{
    ImGui::Begin("Outliner");
    if (!scene.HasScene())
    {
        ImGui::TextDisabled("No scene loaded");
        ImGui::End();
        return false;
    }

    DrawAddMenu(scene, sel, spawnPos);

    if (!ImGui::BeginTable("##outliner", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_NoBordersInBody))
    {
        ImGui::End();
        return false;
    }
    const float eyeW = ImGui::GetTextLineHeight() + 8.0f;
    ImGui::TableSetupColumn("##vis",  ImGuiTableColumnFlags_WidthFixed, eyeW);
    ImGui::TableSetupColumn("Item",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 96.0f);

    using Kind = Selection::Kind;
    DrawRow(sel, Kind::World, 0, scene.currentDesc.name.c_str(), "Level",
            { ICON_MD_PUBLIC, IM_COL32(120, 190, 255, 255) }, nullptr, false);

    DrawRow(sel, Kind::Camera, 0, "Camera", "Camera",
            { ICON_MD_VIDEOCAM, IM_COL32(190, 190, 200, 255) }, nullptr, true);
    DrawRow(sel, Kind::Sun, 0, "Sun", "Directional",
            { ICON_MD_WB_SUNNY, IM_COL32(255, 210, 90, 255) },
            &scene.lights.sunEnabled, true);
    DrawRow(sel, Kind::SpotLight, 0, "Spot Light", "Spot Light",
            { ICON_MD_HIGHLIGHT, IM_COL32(255, 190, 110, 255) },
            &scene.spotLight.enabled, true);
    DrawRow(sel, Kind::Ball, 0, "Ball", "Rigid Body",
            { ICON_MD_LENS, IM_COL32(255, 140, 60, 255) },
            &scene.ballVisible, true);

    bool wantDelete = false;
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        char label[32];
        sprintf_s(label, "Point Light %d", i + 1);
        wantDelete |= DrawRow(sel, Kind::PointLight, i, label, "Point Light",
                { ICON_MD_LIGHTBULB_OUTLINE, IM_COL32(255, 170, 80, 255) },
                &scene.lights.lights[i].enabled, true, true);
    }
    for (int i = 0; i < (int)scene.loadedMeshes.size(); ++i)
        wantDelete |= DrawRow(sel, Kind::Mesh, i, scene.loadedMeshes[i].def.name.c_str(),
                "Static Mesh", { ICON_MD_CATEGORY, IM_COL32(140, 200, 160, 255) },
                &scene.loadedMeshes[i].visible, true, true);
    using ObjType = SE::SceneDescriptor::SceneObject::Type;
    for (int i = 0; i < (int)scene.sceneObjects.size(); ++i)
    {
        const bool isSphere = scene.sceneObjects[i].def.type == ObjType::Sphere;
        char label[32];
        sprintf_s(label, "%s %d", isSphere ? "Sphere" : "Plane", i + 1);
        wantDelete |= DrawRow(sel, Kind::Object, i, label, isSphere ? "Sphere" : "Plane",
                { isSphere ? ICON_MD_PANORAMA_FISH_EYE : ICON_MD_CROP_LANDSCAPE,
                  IM_COL32(150, 210, 220, 255) },
                &scene.sceneObjects[i].visible, true, true);
    }
    for (int i = 0; i < (int)scene.particleSystems.size(); ++i)
    {
        char label[32];
        sprintf_s(label, "Emitter %d", i + 1);
        wantDelete |= DrawRow(sel, Kind::Emitter, i, label, "Emitter",
                { ICON_MD_GRAIN, IM_COL32(210, 150, 255, 255) },
                &scene.particleSystems[i]->enabled, true, true);
    }
    ImGui::EndTable();
    ImGui::End();

    return wantDelete && DeleteSelection(scene, sel);
}

} // namespace Editor
