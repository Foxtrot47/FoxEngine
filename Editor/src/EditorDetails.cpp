#include "EditorDetails.h"
#include "EditorPanels.h"
#include "EditorSettings.h"

#include "Engine/Scene/SceneRuntime.h"
#include <array>
#include <cstring>
#include <functional>

namespace Editor {

namespace {

// Call immediately after an ImGui widget that edits three floats at v.
// Captures the value on activation and pushes an undo entry when the edit
// ends. onApply propagates the restored value to dependent state.
void TrackFloat3Undo(UndoStack& undo, const char* label, float* v,
                     std::function<void()> onApply = {})
{
    static float s_before[3];
    if (ImGui::IsItemActivated())
        std::memcpy(s_before, v, sizeof(s_before));
    if (!ImGui::IsItemDeactivatedAfterEdit())
        return;
    std::array<float, 3> before = { s_before[0], s_before[1], s_before[2] };
    std::array<float, 3> after  = { v[0], v[1], v[2] };
    undo.Push({ label,
                [v, before, onApply] {
                    std::memcpy(v, before.data(), sizeof(float) * 3);
                    if (onApply) onApply();
                },
                [v, after, onApply] {
                    std::memcpy(v, after.data(), sizeof(float) * 3);
                    if (onApply) onApply();
                } });
}

// Keeps the serializable mesh definition in sync with the live transform.
void SyncMeshDef(SE::SceneRuntime::LoadedMesh& loaded)
{
    const auto* t = loaded.transform;
    loaded.def.position = { t->position.x, t->position.y, t->position.z };
    loaded.def.rotation = { t->eulerDeg.x, t->eulerDeg.y, t->eulerDeg.z };
    loaded.def.scale    = { t->scale.x, t->scale.y, t->scale.z };
}

void DrawMeshDetails(SE::SceneRuntime& scene, int index, UndoStack& undo)
{
    auto& loaded = scene.loadedMeshes[index];
    auto* t      = loaded.transform;
    if (!t)
        return;

    ImGui::Text("%s", loaded.def.name.c_str());
    ImGui::TextDisabled("%s", loaded.def.path.c_str());
    ImGui::Separator();

    auto sync = [&scene, index] {
        if (index < (int)scene.loadedMeshes.size())
            SyncMeshDef(scene.loadedMeshes[index]);
    };
    ImGui::DragFloat3("Position", &t->position.x, 0.1f);
    TrackFloat3Undo(undo, "Move", &t->position.x, sync);
    ImGui::DragFloat3("Rotation", &t->eulerDeg.x, 0.5f);
    TrackFloat3Undo(undo, "Rotate", &t->eulerDeg.x, sync);
    ImGui::DragFloat3("Scale", &t->scale.x, 0.01f);
    TrackFloat3Undo(undo, "Scale", &t->scale.x, sync);
    SyncMeshDef(loaded);

    ImGui::Separator();
    ImGui::Checkbox("Cast Shadows", &loaded.def.castShadows);
    ImGui::Checkbox("Receive Shadows", &loaded.def.receiveShadows);
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

void DrawBallDetails(SE::SceneRuntime& scene)
{
    ImGui::Text("Ball (physics body)");
    ImGui::Separator();
    ImGui::DragFloat3("Spawn Pos",    &scene.ballSpawn.x,                1.0f);
    ImGui::SliderFloat("Radius",      &scene.ballRadius,                 0.1f, 10.0f);
    ImGui::SliderFloat("Mass",        &scene.ballRigidBody->mass,        0.1f, 10.0f);
    ImGui::SliderFloat("Restitution", &scene.ballRigidBody->restitution, 0.0f, 1.0f);
    ImGui::SliderFloat("Friction",    &scene.ballRigidBody->friction,    0.0f, 1.0f);
    ImGui::Text("pos (%.1f, %.1f, %.1f)",
        scene.ballTransform->position.x, scene.ballTransform->position.y,
        scene.ballTransform->position.z);
    ImGui::Text("vel (%.2f, %.2f, %.2f)",
        scene.ballRigidBody->velocity.x, scene.ballRigidBody->velocity.y,
        scene.ballRigidBody->velocity.z);
    if (ImGui::Button("Reset"))     scene.ResetBall();
    ImGui::SameLine();
    if (ImGui::Button("Launch up")) scene.ballRigidBody->AddImpulse({ 0.0f, 20.0f, 0.0f });
}

void DrawSelectionDetails(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                          SE::AssetManager& assets, ID3D11Device* device,
                          SE::ShaderLibrary& shaders)
{
    switch (sel.kind)
    {
    case Selection::Kind::World:      DrawWorldProps(scene); break;
    case Selection::Kind::Camera:     DrawCameraDetails(scene); break;
    case Selection::Kind::Sun:        DrawSunProps(scene); break;
    case Selection::Kind::SpotLight:  DrawSpotLightProps(scene); break;
    case Selection::Kind::PointLight:
        if (sel.index < scene.lights.numLights)
            DrawPointLightProps(scene, sel.index);
        break;
    case Selection::Kind::Mesh:
        if (sel.index < (int)scene.loadedMeshes.size())
            DrawMeshDetails(scene, sel.index, undo);
        break;
    case Selection::Kind::Object:
        if (sel.index < (int)scene.sceneObjects.size())
            DrawObjectDetails(scene, sel.index);
        break;
    case Selection::Kind::Emitter:
        DrawEmitterProps(scene, sel.index, assets, device, shaders);
        break;
    case Selection::Kind::Ball:
        DrawBallDetails(scene);
        break;
    default:
        ImGui::TextDisabled("Nothing selected");
        break;
    }
}

} // namespace

void DrawDetailsPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                      SE::AssetManager& assets, ID3D11Device* device,
                      SE::ShaderLibrary& shaders)
{
    ImGui::Begin("Details");
    if (!scene.HasScene())
        ImGui::TextDisabled("No scene loaded");
    else
        DrawSelectionDetails(scene, sel, undo, assets, device, shaders);
    ImGui::End();
}

} // namespace Editor
