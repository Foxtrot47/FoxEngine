#pragma once
#include <d3d11.h>
#include <imgui.h>
#include "EditorSelection.h"

namespace SE {
class SceneRuntime;
class AssetManager;
class ShaderLibrary;
}

namespace Editor {

// Begins a panel window; returns false (window already ended) when no scene
// is loaded.
bool BeginScenePanel(const char* name, SE::SceneRuntime& scene);

// Shared property editors used by the Details panel per selection kind.
void DrawSunProps(SE::SceneRuntime& scene);
void DrawSpotLightProps(SE::SceneRuntime& scene);
void DrawPointLightProps(SE::SceneRuntime& scene, int index);
void DrawEmitterProps(SE::SceneRuntime& scene, int index, SE::AssetManager& assets,
                      ID3D11Device* device, SE::ShaderLibrary& shaders);

// Sun / point light overlay drawn into the viewport panel's draw list,
// projected through the scene camera onto the viewport image rect. Clicking
// an indicator selects that light; returns true when a click was consumed.
bool DrawLightIndicators(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                         Selection& sel);

} // namespace Editor
