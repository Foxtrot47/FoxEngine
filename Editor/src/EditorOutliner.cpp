#include "EditorOutliner.h"
#include "EditorEntityOps.h"
#include "EditorIcons.h"
#include "EditorUndo.h"

#include "Engine/Scene/SceneRuntime.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>

namespace Editor {

namespace {

// Inline-rename state for the row being edited (at most one at a time).
struct RenameState
{
    Selection::Kind kind  = Selection::Kind::None;
    int             index = -1;
    char            buf[128] = {};
    bool            takeFocus = false;
};
RenameState s_rename;
ImGuiTextFilter s_filter;
// Play mode: rows stay selectable for inspection, but nothing is editable.
bool s_readOnly = false;

void BeginRename(Selection::Kind kind, int index, const std::string& current)
{
    s_rename.kind  = kind;
    s_rename.index = index;
    snprintf(s_rename.buf, sizeof(s_rename.buf), "%s", current.c_str());
    s_rename.takeFocus = true;
}

// Persisted display-name storage for the selection, or nullptr when the
// entity kind has no editable name.
std::string* EntityName(SE::SceneRuntime& scene, Selection::Kind kind, int index)
{
    using Kind = Selection::Kind;
    switch (kind)
    {
    case Kind::Mesh:
        return index < (int)scene.loadedMeshes.size()
            ? &scene.loadedMeshes[index].def.name : nullptr;
    case Kind::PointLight:
        return index < (int)scene.pointLightDescs.size()
            ? &scene.pointLightDescs[index].name : nullptr;
    case Kind::Object:
        return index < (int)scene.sceneObjects.size()
            ? &scene.sceneObjects[index].def.name : nullptr;
    case Kind::Emitter:
        return index < (int)scene.particleEmitterDescs.size()
            ? &scene.particleEmitterDescs[index].name : nullptr;
    case Kind::Empty:
        return index < (int)scene.emptyEntities.size()
            ? &scene.emptyEntities[index].def.name : nullptr;
    default:
        return nullptr;
    }
}

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

struct RenameCommit
{
    bool        pending = false;
    Selection   target;
    std::string name;
};

struct RowAction
{
    bool         remove    = false;
    bool         duplicate = false;
    RenameCommit rename;
};

// Inline InputText shown in place of the row label while renaming. Enter or
// focus loss queues the new name for the caller to apply undoably; Escape
// cancels.
void DrawRenameInput(RenameCommit& commit)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (s_rename.takeFocus)
    {
        ImGui::SetKeyboardFocusHere();
        s_rename.takeFocus = false;
    }
    const bool entered = ImGui::InputText("##rename", s_rename.buf,
                                          sizeof(s_rename.buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_AutoSelectAll);
    if (!entered && !ImGui::IsItemDeactivated())
        return;
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && s_rename.buf[0] != '\0')
        commit = { true, { s_rename.kind, s_rename.index }, s_rename.buf };
    s_rename = {};
}

// Rows with a non-null name are editable, UE-style: right-click selects and
// offers Rename / Duplicate / Delete; the returned action is applied by the
// caller after the table is finished.
RowAction DrawRow(Selection& sel, Selection::Kind kind, int index, const char* label,
                  const char* typeName, const IconDesc& icon, bool* visible,
                  bool indent, std::string* name = nullptr)
{
    const std::string searchText = std::string(label) + " " + typeName;
    if (!s_filter.PassFilter(searchText.c_str())) return {};
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID((int)kind);
    ImGui::PushID(index);

    if (indent)
        ImGui::Indent(14.0f);
    const float rowH = ImGui::GetTextLineHeight() + 4.0f;
    RowAction action;
    const bool renaming = name && s_rename.kind == kind && s_rename.index == index;
    if (renaming)
    {
        DrawIconCentered(icon, rowH, rowH);
        ImGui::SameLine(0.0f, 5.0f);
        DrawRenameInput(action.rename);
    }
    else
    {
        if (ImGui::Selectable("##row", sel.Is(kind, index),
                              ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap, { 0.0f, rowH }))
            sel.Set(kind, index);
        if (name && !s_readOnly && ImGui::BeginPopupContextItem("##ctx"))
        {
            sel.Set(kind, index);
            if (ImGui::MenuItem("Rename", "F2"))
                BeginRename(kind, index, *name);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                action.duplicate = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del"))
                action.remove = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 0.0f);
        DrawIconCentered(icon, rowH, rowH);
        ImGui::SameLine(0.0f, 5.0f);
        const ImVec2 textPos = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(
            { textPos.x, textPos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f });
        ImGui::TextUnformatted(label);
    }
    if (indent)
        ImGui::Unindent(14.0f);

    ImGui::TableSetColumnIndex(0);
    ImGui::BeginDisabled(s_readOnly);
    DrawEyeToggle(visible);
    ImGui::EndDisabled();

    ImGui::TableSetColumnIndex(2);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextDisabled("%s", typeName);

    ImGui::PopID();
    ImGui::PopID();
    return action;
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
void DrawAddMenu(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                 const DirectX::XMFLOAT3& spawnPos)
{
    using Kind = Selection::Kind;
    using ObjType = SE::SceneDescriptor::SceneObject::Type;
    ImGui::BeginDisabled(s_readOnly);
    if (ImGui::Button(ICON_MD_ADD " Add"))
        ImGui::OpenPopup("##add_entity");
    ImGui::EndDisabled();
    if (!ImGui::BeginPopup("##add_entity"))
        return;
    Selection spawned;
    ImGui::SeparatorText("General");
    if (IconMenuItem({ ICON_MD_CHECK_BOX_OUTLINE_BLANK, IM_COL32(200, 200, 200, 255) },
                     "Empty Entity") &&
        scene.AddEmpty(spawnPos))
        spawned.Set(Kind::Empty, (int)scene.emptyEntities.size() - 1);
    ImGui::SeparatorText("Shapes");
    if (IconMenuItem({ ICON_MD_PANORAMA_FISH_EYE, IM_COL32(150, 210, 220, 255) },
                     "Sphere") &&
        scene.AddSceneObject(ObjType::Sphere, spawnPos))
        spawned.Set(Kind::Object, (int)scene.sceneObjects.size() - 1);
    if (IconMenuItem({ ICON_MD_CROP_LANDSCAPE, IM_COL32(150, 210, 220, 255) },
                     "Plane") &&
        scene.AddSceneObject(ObjType::Plane, spawnPos))
        spawned.Set(Kind::Object, (int)scene.sceneObjects.size() - 1);
    ImGui::SeparatorText("Lights");
    if (IconMenuItem({ ICON_MD_LIGHTBULB_OUTLINE, IM_COL32(255, 170, 80, 255) },
                     "Point Light", scene.lights.numLights < 8) &&
        scene.AddPointLight(spawnPos))
        spawned.Set(Kind::PointLight, scene.lights.numLights - 1);
    ImGui::SeparatorText("Effects");
    if (IconMenuItem({ ICON_MD_GRAIN, IM_COL32(210, 150, 255, 255) },
                     "Particle Emitter") &&
        scene.AddParticleEmitter(spawnPos))
        spawned.Set(Kind::Emitter, (int)scene.particleSystems.size() - 1);
    ImGui::EndPopup();
    if (spawned.kind == Kind::None)
        return;
    sel = spawned;
    PushSpawnUndo(scene, undo, sel, spawned, "Add");
}

void ApplyRename(SE::SceneRuntime& scene, UndoStack& undo, const RenameCommit& commit)
{
    std::string* name = EntityName(scene, commit.target.kind, commit.target.index);
    if (!name || *name == commit.name)
        return;
    const Selection target = commit.target;
    const std::string before = *name;
    const std::string after  = commit.name;
    *name = after;
    auto set = [&scene, target](const std::string& value) {
        if (std::string* n = EntityName(scene, target.kind, target.index))
            *n = value;
    };
    undo.Push({ "Rename",
                [set, before] { set(before); },
                [set, after]  { set(after); } });
}

// Point light, mesh, object, and emitter rows with editable names.
RowAction DrawEntityRows(SE::SceneRuntime& scene, Selection& sel)
{
    using Kind = Selection::Kind;
    RowAction action;
    auto merge = [&action](const RowAction& a) {
        action.remove |= a.remove;
        action.duplicate |= a.duplicate;
        if (a.rename.pending)
            action.rename = a.rename;
    };
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        std::string& name = scene.pointLightDescs[i].name;
        char label[32];
        sprintf_s(label, "Point Light %d", i + 1);
        merge(DrawRow(sel, Kind::PointLight, i,
                name.empty() ? label : name.c_str(), "Point Light",
                { ICON_MD_LIGHTBULB_OUTLINE, IM_COL32(255, 170, 80, 255) },
                &scene.lights.lights[i].enabled, true, &name));
    }
    for (int i = 0; i < (int)scene.loadedMeshes.size(); ++i)
        merge(DrawRow(sel, Kind::Mesh, i, scene.loadedMeshes[i].def.name.c_str(),
                "Static Mesh", { ICON_MD_CATEGORY, IM_COL32(140, 200, 160, 255) },
                &scene.loadedMeshes[i].visible, true,
                &scene.loadedMeshes[i].def.name));
    using ObjType = SE::SceneDescriptor::SceneObject::Type;
    for (int i = 0; i < (int)scene.sceneObjects.size(); ++i)
    {
        const bool isSphere = scene.sceneObjects[i].def.type == ObjType::Sphere;
        std::string& name = scene.sceneObjects[i].def.name;
        char label[32];
        sprintf_s(label, "%s %d", isSphere ? "Sphere" : "Plane", i + 1);
        merge(DrawRow(sel, Kind::Object, i,
                name.empty() ? label : name.c_str(), isSphere ? "Sphere" : "Plane",
                { isSphere ? ICON_MD_PANORAMA_FISH_EYE : ICON_MD_CROP_LANDSCAPE,
                  IM_COL32(150, 210, 220, 255) },
                &scene.sceneObjects[i].visible, true, &name));
    }
    for (int i = 0; i < (int)scene.particleSystems.size(); ++i)
    {
        std::string* name = i < (int)scene.particleEmitterDescs.size()
            ? &scene.particleEmitterDescs[i].name : nullptr;
        char label[32];
        sprintf_s(label, "Emitter %d", i + 1);
        merge(DrawRow(sel, Kind::Emitter, i,
                (name && !name->empty()) ? name->c_str() : label, "Emitter",
                { ICON_MD_GRAIN, IM_COL32(210, 150, 255, 255) },
                &scene.particleSystems[i]->enabled, true, name));
    }
    for (int i = 0; i < (int)scene.emptyEntities.size(); ++i)
    {
        std::string& name = scene.emptyEntities[i].def.name;
        char label[32];
        sprintf_s(label, "Empty %d", i + 1);
        merge(DrawRow(sel, Kind::Empty, i, name.empty() ? label : name.c_str(), "Empty",
                { ICON_MD_CHECK_BOX_OUTLINE_BLANK, IM_COL32(200, 200, 200, 255) },
                nullptr, true, &name));
    }
    return action;
}

} // namespace

void RequestRenameSelection(SE::SceneRuntime& scene, const Selection& sel)
{
    if (std::string* name = EntityName(scene, sel.kind, sel.index))
        BeginRename(sel.kind, sel.index, *name);
}

void DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                       const DirectX::XMFLOAT3& spawnPos, bool playing)
{
    ImGui::Begin("Outliner");
    if (!scene.HasScene())
    {
        ImGui::TextDisabled("No scene loaded");
        ImGui::End();
        return;
    }

    s_readOnly = playing;
    if (playing)
    {
        s_rename = {};
        ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "Playing");
        ImGui::SameLine();
    }
    DrawAddMenu(scene, sel, undo, spawnPos);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "Search actors or types...", s_filter.InputBuf,
                                  IM_ARRAYSIZE(s_filter.InputBuf)))
        s_filter.Build();

    if (!ImGui::BeginTable("##outliner", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_NoBordersInBody))
    {
        ImGui::End();
        return;
    }
    const float eyeW = ImGui::GetTextLineHeight() + 8.0f;
    ImGui::TableSetupColumn("##vis",  ImGuiTableColumnFlags_WidthFixed, eyeW);
    ImGui::TableSetupColumn("Item",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Defensive: keep the desc mirror sized so names stay addressable.
    if ((int)scene.pointLightDescs.size() < scene.lights.numLights)
        scene.pointLightDescs.resize(scene.lights.numLights);

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

    const RowAction action = DrawEntityRows(scene, sel);
    ImGui::EndTable();
    ImGui::End();

    if (playing)
        return;
    if (action.rename.pending)
        ApplyRename(scene, undo, action.rename);
    if (action.duplicate)
        DuplicateSelection(scene, sel, undo);
    if (action.remove)
        DeleteSelection(scene, sel, undo);
}

} // namespace Editor
