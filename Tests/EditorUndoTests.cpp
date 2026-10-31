#include "EditorEntityOps.h"
#include "EditorUndo.h"
#include "Engine/Scene/SceneRuntime.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using Kind = Editor::Selection::Kind;

namespace {
void Require(bool value, const char* message)
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

// Order-sensitive fingerprints of every structural entity list.
std::string Lights(const SE::SceneRuntime& s)
{
    std::string out;
    for (int i = 0; i < s.lights.numLights; ++i)
        out += s.pointLightDescs[i].name + (s.lightCastsShadow[i] ? "+" : "-") +
               std::to_string(static_cast<int>(s.lights.lights[i].position.x)) + ",";
    return out;
}
std::string Objects(const SE::SceneRuntime& s)
{
    std::string out;
    for (const auto& o : s.sceneObjects) out += o.def.name + ",";
    return out;
}
std::string Meshes(const SE::SceneRuntime& s)
{
    std::string out;
    for (const auto& m : s.loadedMeshes) out += m.def.name + ",";
    return out;
}
std::string Emitters(const SE::SceneRuntime& s)
{
    std::string out;
    for (size_t i = 0; i < s.particleSystems.size(); ++i)
        out += s.particleEmitterDescs[i].name + ":" +
               std::to_string(s.selectedParticlePreset[i]) +
               (s.particleSystems[i] ? "" : "!null") + ",";
    return out;
}
std::string All(const SE::SceneRuntime& s)
{
    return Lights(s) + "|" + Objects(s) + "|" + Meshes(s) + "|" + Emitters(s);
}

// Three entities of every deletable kind with distinguishable state. GPU
// resources are not needed: emitters are left uninitialized.
void Populate(SE::SceneRuntime& scene)
{
    for (int i = 0; i < 3; ++i)
    {
        const std::string n = std::to_string(i);
        Require(scene.AddPointLight({ static_cast<float>(i), 0.0f, 0.0f }), "add light");
        scene.pointLightDescs[i].name = "L" + n;
        scene.lightCastsShadow[i] = (i == 1);

        SE::SceneRuntime::LoadedObject object;
        object.def.name = "O" + n;
        scene.sceneObjects.push_back(object);

        SE::SceneRuntime::LoadedMesh mesh;
        mesh.def.name = "M" + n;
        scene.loadedMeshes.push_back(mesh);

        scene.particleSystems.push_back(std::make_unique<SE::ParticleSystem>());
        SE::SceneDescriptor::ParticleEmitterDesc desc;
        desc.name = "E" + n;
        scene.particleEmitterDescs.push_back(desc);
        scene.selectedParticlePreset.push_back(10 + i);
    }
}

void TestDeleteUndoRestoresIndices()
{
    SE::SceneRuntime scene;
    Populate(scene);
    const std::string initial = All(scene);
    Editor::UndoStack undo;
    Editor::Selection sel;

    for (Kind kind : { Kind::PointLight, Kind::Object, Kind::Mesh, Kind::Emitter })
    {
        sel.Set(kind, 1);
        Require(Editor::DeleteSelection(scene, sel, undo), "delete middle entity");
        Require(sel.kind == Kind::None, "delete clears selection");
    }
    const std::string deleted = All(scene);
    Require(deleted == "L0-0,L2-2,|O0,O2,|M0,M2,|E0:10,E2:12,", "middle entities removed");

    while (undo.CanUndo()) undo.Undo();
    Require(All(scene) == initial, "undo reinserts at original index with full state");
    Require(sel.Is(Kind::PointLight, 1), "undoing a delete selects the restored entity");

    while (undo.CanRedo()) undo.Redo();
    Require(All(scene) == deleted, "redo deletes the same entities again");
}

// Entries recorded after a structural edit address indices that only exist
// in that state; undoing in order must keep them consistent.
void TestDuplicateAndSpawnRoundTrip()
{
    SE::SceneRuntime scene;
    Populate(scene);
    Editor::UndoStack undo;
    Editor::Selection sel;

    sel.Set(Kind::PointLight, 1);
    Require(Editor::DeleteSelection(scene, sel, undo), "delete L1");
    sel.Set(Kind::PointLight, 0);
    Require(Editor::DuplicateSelection(scene, sel, undo), "duplicate L0");
    Require(sel.Is(Kind::PointLight, 2), "duplicate selects the appended copy");
    Require(Lights(scene) == "L0-0,L2-2,L0-0,", "duplicate appended");

    SE::SceneRuntime::LoadedObject spawned;
    spawned.def.name = "New";
    scene.sceneObjects.push_back(spawned);
    sel.Set(Kind::Object, static_cast<int>(scene.sceneObjects.size()) - 1);
    Editor::PushSpawnUndo(scene, undo, sel, sel, "Add");
    Require(std::string(undo.UndoLabel()) == "Add", "spawn label");

    undo.Undo();
    Require(Objects(scene) == "O0,O1,O2,", "undo spawn removes it");
    Require(sel.kind == Kind::None, "undo spawn clears same-kind selection");
    undo.Undo();
    Require(Lights(scene) == "L0-0,L2-2,", "undo duplicate removes the copy");
    undo.Undo();
    Require(Lights(scene) == "L0-0,L1+1,L2-2,", "undo delete restores L1 in place");

    undo.Redo();
    undo.Redo();
    undo.Redo();
    Require(Lights(scene) == "L0-0,L2-2,L0-0,", "redo replays delete + duplicate");
    Require(Objects(scene) == "O0,O1,O2,New,", "redo restores the spawned object");
    Require(sel.Is(Kind::Object, 3), "redo spawn selects it");
}

void TestLightCapacityRoundTrip()
{
    SE::SceneRuntime scene;
    while (scene.lights.numLights < 8)
        Require(scene.AddPointLight({ 0.0f, 0.0f, 0.0f }), "fill lights");
    scene.pointLightDescs[0].name = "first";
    Editor::UndoStack undo;
    Editor::Selection sel;
    sel.Set(Kind::PointLight, 0);
    Require(Editor::DeleteSelection(scene, sel, undo), "delete at capacity");
    Require(scene.lights.numLights == 7, "light removed");
    undo.Undo();
    Require(scene.lights.numLights == 8 && scene.pointLightDescs[0].name == "first",
            "undo refills the full light table at the original slot");
}
}

int main()
{
    TestDeleteUndoRestoresIndices();
    TestDuplicateAndSpawnRoundTrip();
    TestLightCapacityRoundTrip();
    std::puts("EditorUndoTests: all three groups passed");
    return 0;
}
