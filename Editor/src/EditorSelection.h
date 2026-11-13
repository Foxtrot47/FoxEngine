#pragma once

namespace Editor {

// Identifies the currently selected scene entity across the outliner,
// details panel, viewport picking, and gizmo.
struct Selection
{
    enum class Kind { None, World, Camera, Sun, SpotLight, PointLight, Mesh,
                      Object, Emitter, Empty };

    Kind kind  = Kind::None;
    int  index = 0;

    bool Is(Kind k, int i = 0) const { return kind == k && index == i; }
    void Set(Kind k, int i = 0)      { kind = k; index = i; }
    void Clear()                     { kind = Kind::None; index = 0; }
};

} // namespace Editor
