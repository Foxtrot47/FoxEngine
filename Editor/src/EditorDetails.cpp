#include "EditorDetails.h"
#include "EditorPanels.h"
#include "EditorPropertyUndo.h"
#include "EditorScripts.h"
#include "EditorSettings.h"

#include "Engine/Scene/SceneRuntime.h"

namespace Editor {

namespace {

ImGuiTextFilter     s_detailsFilter;
PropertyUndoTracker s_propertyUndo;

// The search filters coherent property groups, so dependencies (e.g. enable
// toggles and the settings they reveal) stay together.
bool DetailsMatch(Selection::Kind kind)
{
    using Kind = Selection::Kind;
    switch (kind)
    {
    case Kind::World: return true; // world categories filter individually
    case Kind::Camera: return s_detailsFilter.PassFilter("Camera FOV Near Far Eye Projection");
    case Kind::Sun: return s_detailsFilter.PassFilter("Sun Lighting Elevation Azimuth Intensity Color IBL Shadow");
    case Kind::SpotLight: return s_detailsFilter.PassFilter("Spot Light Position Direction Color Range Angle Intensity Shadow");
    case Kind::PointLight: return s_detailsFilter.PassFilter("Point Light Position Color Radius Flicker Shadow");
    case Kind::Mesh: return s_detailsFilter.PassFilter("Mesh Transform Position Rotation Scale Rendering Shadows");
    case Kind::Object: return s_detailsFilter.PassFilter("Object Transform Position Size Radius Material Tint Metallic Roughness Emissive");
    case Kind::Emitter: return s_detailsFilter.PassFilter("Emitter Particles Preset Emission Transform Lifetime Size Color Velocity Physics Texture");
    case Kind::Empty: return s_detailsFilter.PassFilter("Empty Transform Position Rotation Scale");
    case Kind::Actor: return s_detailsFilter.PassFilter("Actor Class Transform Position Rotation Scale Properties Components Scripts");
    default: return true;
    }
}

// Keeps the serializable mesh definition in sync with the live transform.
void SyncMeshDef(SE::SceneRuntime::LoadedMesh& loaded)
{
    const auto* t = loaded.transform;
    loaded.def.position = { t->position.x, t->position.y, t->position.z };
    loaded.def.rotation = { t->eulerDeg.x, t->eulerDeg.y, t->eulerDeg.z };
    loaded.def.scale    = { t->scale.x, t->scale.y, t->scale.z };
}

void DrawMeshDetails(SE::SceneRuntime& scene, int index)
{
    if (index < 0 || index >= static_cast<int>(scene.loadedMeshes.size())) return;
    auto& loaded = scene.loadedMeshes[index];
    auto* t      = loaded.transform;
    if (!t)
        return;

    ImGui::Text("%s", loaded.def.name.c_str());
    ImGui::TextDisabled("%s", loaded.def.path.c_str());
    ImGui::Separator();

    ImGui::DragFloat3("Position", &t->position.x, 0.1f);
    ImGui::DragFloat3("Rotation", &t->eulerDeg.x, 0.5f);
    ImGui::DragFloat3("Scale", &t->scale.x, 0.01f);
    SyncMeshDef(loaded);

    ImGui::Separator();
    ImGui::Checkbox("Cast Shadows", &loaded.def.castShadows);
    if (ImGui::Checkbox("Receive Shadows", &loaded.def.receiveShadows))
        for (auto& material : loaded.materials)
            material.receiveShadows = loaded.def.receiveShadows;
    ImGui::Checkbox("Visible (off = collider only)", &loaded.def.visible);
}

void DrawEmptyDetails(SE::SceneRuntime& scene, int index)
{
    if (index < 0 || index >= static_cast<int>(scene.emptyEntities.size())) return;
    auto& empty = scene.emptyEntities[index];
    auto* t     = empty.transform;
    if (!t)
        return;

    ImGui::Text("%s", empty.def.name.c_str());
    ImGui::TextDisabled("Empty Entity");
    ImGui::Separator();

    ImGui::DragFloat3("Position", &t->position.x, 0.1f);
    ImGui::DragFloat3("Rotation", &t->eulerDeg.x, 0.5f);
    ImGui::DragFloat3("Scale", &t->scale.x, 0.01f);
    empty.def.position = { t->position.x, t->position.y, t->position.z };
    empty.def.rotation = { t->eulerDeg.x, t->eulerDeg.y, t->eulerDeg.z };
    empty.def.scale    = { t->scale.x, t->scale.y, t->scale.z };
}

void DrawCameraDetails(SE::SceneRuntime& scene)
{
    ImGui::SliderFloat("FOV",    &scene.camera->fovDeg, 20.0f, 120.0f, "%.0f deg");
    ImGui::SliderFloat("Near Z", &scene.camera->nearZ, 0.001f, 10.0f, "%.3f");
    ImGui::SliderFloat("Far Z",  &scene.camera->farZ, 100.0f, 20000.0f, "%.0f");
    ImGui::Text("Eye (%.1f, %.1f, %.1f)",
        scene.camera->eye.x, scene.camera->eye.y, scene.camera->eye.z);
}

void DrawObjectDetails(SE::SceneRuntime& scene, int index)
{
    if (index < 0 || index >= static_cast<int>(scene.sceneObjects.size())) return;
    auto& def = scene.sceneObjects[index].def;
    using Type = SE::SceneDescriptor::SceneObject::Type;
    ImGui::Text("%s", def.type == Type::Sphere ? "Sphere" : "Plane");
    ImGui::Separator();
    ImGui::DragFloat3("Position", def.position.data(), 0.1f);
    if (def.type == Type::Sphere)
        ImGui::SliderFloat("Radius", &def.radius, 0.1f, 20.0f);
    else
    {
        ImGui::SliderFloat("Half Size X", &def.halfSizeX, 0.5f, 100.0f);
        ImGui::SliderFloat("Half Size Z", &def.halfSizeZ, 0.5f, 100.0f);
    }
    ImGui::Separator();
    ImGui::ColorEdit3("Tint", def.tint.data());
    ImGui::SliderFloat("Metallic Scale",  &def.metallicScale, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness Scale", &def.roughnessScale, 0.0f, 2.0f);
    ImGui::ColorEdit3("Emissive Color", def.emissiveColor.data());
    ImGui::SliderFloat("Emissive Intensity", &def.emissiveIntensity, 0.0f, 20.0f);
}

// Component overrides only take effect when the actor is rebuilt, which
// waits for the edit gesture to end (a drag or text entry rebuilds once).
struct PendingActorRebuild
{
    bool pending = false;
    int  index   = -1;
};
PendingActorRebuild s_actorRebuild;

// Returns true when a non-widget edit (add/remove script, reset) happened.
bool DrawActorDetails(SE::SceneRuntime& scene, int index, ScriptCatalog& catalog)
{
    if (index < 0 || index >= static_cast<int>(scene.actors.size())) return false;
    auto& actor = scene.actors[index];
    auto* t     = actor.transform;
    if (!t)
        return false;

    ImGui::Text("%s", actor.def.name.c_str());
    ImGui::TextDisabled("%s%s", actor.def.actorClass.c_str(), actor.spawned ? " (spawned)" : "");
    if (!actor.def.resolveError.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", actor.def.resolveError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();

    ImGui::DragFloat3("Position", &t->position.x, 0.1f);
    ImGui::DragFloat3("Rotation", &t->eulerDeg.x, 0.5f);
    ImGui::DragFloat3("Scale", &t->scale.x, 0.01f);

    bool changed = DrawActorClassProperties(actor.def, catalog, scene);

    ComponentTransformEdit transformEdit;
    if (DrawActorComponents(actor.def, catalog, scene, transformEdit))
    {
        s_actorRebuild = { true, index };
        changed = true;
    }
    if (transformEdit.part >= 0 && transformEdit.part < static_cast<int>(actor.parts.size()))
    {
        // Preview while dragging; the rebuild re-creates bodies at the new pose.
        auto* part = actor.parts[transformEdit.part].transform;
        part->position = { transformEdit.position[0], transformEdit.position[1], transformEdit.position[2] };
        part->eulerDeg = { transformEdit.rotation[0], transformEdit.rotation[1], transformEdit.rotation[2] };
        part->scale    = { transformEdit.scale[0], transformEdit.scale[1], transformEdit.scale[2] };
    }

    changed |= DrawScriptComponents(actor.def.scripts, catalog, scene);
    return changed;
}

void DrawSelectionDetails(SE::SceneRuntime& scene, Selection& sel,
                          SE::AssetManager& assets, ID3D11Device* device,
                          SE::ShaderLibrary& shaders, uint32_t outWidth, uint32_t outHeight)
{
    switch (sel.kind)
    {
    case Selection::Kind::World:      DrawWorldProps(scene, outWidth, outHeight, s_detailsFilter); break;
    case Selection::Kind::Camera:     DrawCameraDetails(scene); break;
    case Selection::Kind::Sun:        DrawSunProps(scene); break;
    case Selection::Kind::SpotLight:  DrawSpotLightProps(scene); break;
    case Selection::Kind::PointLight: DrawPointLightProps(scene, sel.index); break;
    case Selection::Kind::Mesh: DrawMeshDetails(scene, sel.index); break;
    case Selection::Kind::Object: DrawObjectDetails(scene, sel.index); break;
    case Selection::Kind::Emitter:
        DrawEmitterProps(scene, sel.index, assets, device, shaders);
        break;
    case Selection::Kind::Empty: DrawEmptyDetails(scene, sel.index); break;
    case Selection::Kind::Actor: break; // DrawActorDetails: it needs the script catalog
    default:
        ImGui::TextDisabled("Nothing selected");
        break;
    }
}

void ApplyPendingActorRebuild(SE::SceneRuntime& scene)
{
    if (!s_actorRebuild.pending || ImGui::IsAnyItemActive())
        return;
    scene.RebuildActor(s_actorRebuild.index);
    s_actorRebuild = {};
}

} // namespace

void DrawDetailsPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                      SE::AssetManager& assets, ID3D11Device* device,
                      SE::ShaderLibrary& shaders, uint32_t outWidth, uint32_t outHeight,
                      ScriptCatalog& scripts, bool playing)
{
    ImGui::Begin("Details");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "Search property groups...", s_detailsFilter.InputBuf,
                                  IM_ARRAYSIZE(s_detailsFilter.InputBuf)))
        s_detailsFilter.Build();
    ImGui::Separator();
    if (!scene.HasScene())
        ImGui::TextDisabled("No scene loaded");
    else if (!DetailsMatch(sel.kind))
        ImGui::TextDisabled("No matching property groups");
    else
    {
        // Play mode shows the live play-session values read-only; edits
        // belong to the edit state restored on Stop.
        if (playing)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1.0f), "Playing: stop (F5) to edit");
            ImGui::BeginDisabled();
        }
        s_propertyUndo.Begin(scene, sel);
        DrawSelectionDetails(scene, sel, assets, device, shaders, outWidth, outHeight);
        if (sel.kind == Selection::Kind::Actor && DrawActorDetails(scene, sel.index, scripts))
            s_propertyUndo.MarkEdited();
        // Before End, so the undo entry's "after" state is the rebuilt actor.
        ApplyPendingActorRebuild(scene);
        s_propertyUndo.End(scene, undo, device, shaders);
        if (playing)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace Editor
