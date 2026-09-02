#include "EditorOutliner.h"

#include "Engine/Scene/SceneRuntime.h"
#include <imgui.h>
#include <cstdio>

namespace Editor {

namespace {

void SelectableRow(const char* label, Selection& sel, Selection::Kind kind, int index = 0)
{
    if (ImGui::Selectable(label, sel.Is(kind, index)))
        sel.Set(kind, index);
}

void DrawPointLightRows(SE::SceneRuntime& scene, Selection& sel)
{
    if (scene.lights.numLights <= 0)
        return;
    if (!ImGui::TreeNodeEx("Point Lights", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        char label[32];
        sprintf_s(label, "Point Light %d", i + 1);
        SelectableRow(label, sel, Selection::Kind::PointLight, i);
    }
    ImGui::TreePop();
}

void DrawMeshRows(SE::SceneRuntime& scene, Selection& sel)
{
    if (scene.loadedMeshes.empty())
        return;
    if (!ImGui::TreeNodeEx("Meshes", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    for (int i = 0; i < (int)scene.loadedMeshes.size(); ++i)
    {
        ImGui::PushID(i);
        SelectableRow(scene.loadedMeshes[i].def.name.c_str(), sel,
                      Selection::Kind::Mesh, i);
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void DrawObjectRows(SE::SceneRuntime& scene, Selection& sel)
{
    if (scene.sceneObjects.empty())
        return;
    if (!ImGui::TreeNodeEx("Objects", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    using Type = SE::SceneDescriptor::SceneObject::Type;
    for (int i = 0; i < (int)scene.sceneObjects.size(); ++i)
    {
        char label[32];
        sprintf_s(label, "%s %d",
                  scene.sceneObjects[i].def.type == Type::Sphere ? "Sphere" : "Plane",
                  i + 1);
        SelectableRow(label, sel, Selection::Kind::Object, i);
    }
    ImGui::TreePop();
}

void DrawEmitterRows(SE::SceneRuntime& scene, Selection& sel)
{
    if (scene.particleSystems.empty())
        return;
    if (!ImGui::TreeNodeEx("Particle Emitters", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    for (int i = 0; i < (int)scene.particleSystems.size(); ++i)
    {
        char label[32];
        sprintf_s(label, "Emitter %d", i + 1);
        SelectableRow(label, sel, Selection::Kind::Emitter, i);
    }
    ImGui::TreePop();
}

} // namespace

void DrawOutlinerPanel(SE::SceneRuntime& scene, Selection& sel)
{
    ImGui::Begin("Outliner");
    if (!scene.HasScene())
    {
        ImGui::TextDisabled("No scene loaded");
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("%s", scene.currentDesc.name.c_str());
    ImGui::Separator();
    SelectableRow("Camera", sel, Selection::Kind::Camera);
    SelectableRow("Sun", sel, Selection::Kind::Sun);
    SelectableRow("Spot Light", sel, Selection::Kind::SpotLight);
    DrawPointLightRows(scene, sel);
    DrawMeshRows(scene, sel);
    DrawObjectRows(scene, sel);
    DrawEmitterRows(scene, sel);
    ImGui::End();
}

} // namespace Editor
