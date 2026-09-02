#pragma once
#include <cstdint>
#include <d3d11.h>
#include <imgui.h>

namespace SE {
class SceneRuntime;
class AssetManager;
class ShaderLibrary;
}

namespace Editor {

class EditorCamera;

// Scene tuning panels. Each panel owns its own ImGui::Begin/End and shows a
// placeholder when no scene is loaded.
void DrawCameraPanel(SE::SceneRuntime& scene, EditorCamera& cam);
void DrawMaterialPanel(SE::SceneRuntime& scene);
void DrawPhysicsPanel(SE::SceneRuntime& scene);
void DrawLightingPanel(SE::SceneRuntime& scene);
void DrawPostProcessPanel(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight);
void DrawParticlesPanel(SE::SceneRuntime& scene, SE::AssetManager& assets,
                        ID3D11Device* device, SE::ShaderLibrary& shaders);

// Sun / point light overlay drawn into the viewport panel's draw list,
// projected through the scene camera onto the viewport image rect.
void DrawLightIndicators(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax);

} // namespace Editor
