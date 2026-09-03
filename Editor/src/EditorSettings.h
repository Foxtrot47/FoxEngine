#pragma once
#include <cstdint>

namespace SE {
class SceneRuntime;
}

namespace Editor {

// "Settings" page: the global post-process chain (tone mapping, bloom,
// SSAO, fog, temporal AA / upscaling, FXAA).
void DrawSettingsPanel(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight);

// World-level properties shown in Details when the level root is selected:
// global material overrides, world physics, and lighting/debug toggles.
void DrawWorldProps(SE::SceneRuntime& scene);

} // namespace Editor
