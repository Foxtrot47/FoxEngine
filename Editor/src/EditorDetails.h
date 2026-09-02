#pragma once
#include <d3d11.h>
#include "EditorSelection.h"
#include "EditorUndo.h"

namespace SE {
class SceneRuntime;
class AssetManager;
class ShaderLibrary;
}

namespace Editor {

// Property inspector for the current selection (UE Details-style). Transform
// drags push undo entries; other property edits are immediate.
void DrawDetailsPanel(SE::SceneRuntime& scene, Selection& sel, UndoStack& undo,
                      SE::AssetManager& assets, ID3D11Device* device,
                      SE::ShaderLibrary& shaders);

} // namespace Editor
