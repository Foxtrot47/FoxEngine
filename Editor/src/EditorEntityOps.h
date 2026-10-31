#pragma once
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

class UndoStack;

// Undoable structural scene edits shared by the outliner, hotkeys and the
// content browser. Undo/redo restores entities at their exact index, so
// index-based entries recorded earlier in the stack stay valid.

// Records an entity that was just appended at spawned.index; undo removes
// it again and redo restores the same instance.
void PushSpawnUndo(SE::SceneRuntime& scene, UndoStack& undo, Selection& sel,
                   Selection spawned, const char* label);

// Removes the selected entity when its kind is deletable (point light,
// mesh, object, emitter) and clears the selection. Returns true on delete.
bool DeleteSelection(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo);

// Appends a copy of the selected entity and moves the selection to it.
bool DuplicateSelection(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo);

} // namespace Editor
