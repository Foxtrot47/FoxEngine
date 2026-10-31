#include "EditorEntityOps.h"
#include "EditorUndo.h"

#include "Engine/Scene/SceneRuntime.h"
#include <memory>

namespace Editor {

namespace {

using Kind = Selection::Kind;

// One removed entity of any deletable kind; only the member matching the
// selection kind is populated.
struct DetachedEntity
{
    SE::SceneRuntime::DetachedPointLight light;
    SE::SceneRuntime::DetachedEmitter    emitter;
    SE::SceneRuntime::LoadedObject       object;
    SE::SceneRuntime::LoadedMesh         mesh;
};

bool IsStructural(Kind kind)
{
    return kind == Kind::PointLight || kind == Kind::Mesh ||
           kind == Kind::Object || kind == Kind::Emitter;
}

void Detach(SE::SceneRuntime& scene, Selection target, DetachedEntity& out)
{
    switch (target.kind)
    {
    case Kind::PointLight: out.light   = scene.RemovePointLight(target.index); break;
    case Kind::Emitter:    out.emitter = scene.RemoveParticleEmitter(target.index); break;
    case Kind::Object:     out.object  = scene.RemoveSceneObject(target.index); break;
    case Kind::Mesh:       out.mesh    = scene.RemoveMesh(target.index); break;
    default: break;
    }
}

void Attach(SE::SceneRuntime& scene, Selection target, DetachedEntity& in)
{
    switch (target.kind)
    {
    case Kind::PointLight: scene.InsertPointLight(target.index, in.light); break;
    case Kind::Emitter:    scene.InsertParticleEmitter(target.index, std::move(in.emitter)); break;
    case Kind::Object:     scene.InsertSceneObject(target.index, std::move(in.object)); break;
    case Kind::Mesh:       scene.InsertMesh(target.index, std::move(in.mesh)); break;
    default: break;
    }
}

// Removing or inserting shifts every later index of the same kind, so a
// same-kind selection can no longer be trusted.
void OnRemoved(Selection& sel, Selection removed)
{
    if (sel.kind == removed.kind)
        sel.Clear();
}

} // namespace

void PushSpawnUndo(SE::SceneRuntime& scene, UndoStack& undo, Selection& sel,
                   Selection spawned, const char* label)
{
    if (!IsStructural(spawned.kind))
        return;
    auto slot = std::make_shared<DetachedEntity>();
    Selection* selection = &sel;
    undo.Push({ label,
                [&scene, selection, spawned, slot] {
                    Detach(scene, spawned, *slot);
                    OnRemoved(*selection, spawned);
                },
                [&scene, selection, spawned, slot] {
                    Attach(scene, spawned, *slot);
                    *selection = spawned;
                } });
}

bool DeleteSelection(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo)
{
    if (!IsStructural(sel.kind))
        return false;
    const Selection removed = sel;
    auto slot = std::make_shared<DetachedEntity>();
    Detach(scene, removed, *slot);
    sel.Clear();

    Selection* selection = &sel;
    undo.Push({ "Delete",
                [&scene, selection, removed, slot] {
                    Attach(scene, removed, *slot);
                    *selection = removed;
                },
                [&scene, selection, removed, slot] {
                    Detach(scene, removed, *slot);
                    OnRemoved(*selection, removed);
                } });
    return true;
}

bool DuplicateSelection(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo)
{
    int newIndex = -1;
    switch (sel.kind)
    {
    case Kind::PointLight: newIndex = scene.DuplicatePointLight(sel.index); break;
    case Kind::Mesh:       newIndex = scene.DuplicateMesh(sel.index); break;
    case Kind::Object:     newIndex = scene.DuplicateSceneObject(sel.index); break;
    case Kind::Emitter:    newIndex = scene.DuplicateParticleEmitter(sel.index); break;
    default: return false;
    }
    if (newIndex < 0)
        return false;
    sel.Set(sel.kind, newIndex);
    PushSpawnUndo(scene, undo, sel, sel, "Duplicate");
    return true;
}

} // namespace Editor
