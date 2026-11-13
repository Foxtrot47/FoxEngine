#pragma once
#include <cstdint>
#include <d3d11.h>
#include "EditorSelection.h"
#include "EditorUndo.h"

namespace SE {
class SceneRuntime;
class AssetManager;
class ShaderLibrary;
}

namespace Editor {

class ScriptCatalog;

// Property inspector for the current selection (UE Details-style), including
// the entity's script components. Edits push undo entries once the gesture
// ends. outWidth and outHeight are the viewport dimensions (used by world FSR
// settings). While playing everything is shown read-only.
void DrawDetailsPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                      SE::AssetManager& assets, ID3D11Device* device,
                      SE::ShaderLibrary& shaders, uint32_t outWidth, uint32_t outHeight,
                      ScriptCatalog& scripts, bool playing);

} // namespace Editor
