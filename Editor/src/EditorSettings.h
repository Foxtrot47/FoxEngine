#pragma once
#include <cstdint>
#include <imgui.h>

namespace SE {
class SceneRuntime;
}

namespace Editor {

// World-level properties shown in Details when the level root is selected:
// global material overrides, world physics, lighting/debug toggles, and the
// post-process chain (all saved with the level). outWidth/outHeight are the
// viewport dimensions used by the FSR upscaling readout.
void DrawWorldProps(SE::SceneRuntime& scene, uint32_t outWidth, uint32_t outHeight,
                    const ImGuiTextFilter& filter);

} // namespace Editor
