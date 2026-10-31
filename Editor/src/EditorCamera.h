#pragma once
#include <windows.h>
#include <DirectXMath.h>

namespace SE { class InputManager; struct CameraComponent; }

namespace Editor {

// Unreal-style navigation. View presets change orientation/projection only;
// F is the explicit frame-selection command. Orthographic orbit stays ortho.
class EditorCamera
{
public:
    enum class ViewMode { Perspective, Isometric, Top, Bottom, Front, Back, Left, Right, Orthographic };
    float moveSpeed = 20.0f;
    float sensitivity = 0.15f;
    float orbitDistance = 10.0f;

    void SyncFromCamera(const SE::CameraComponent& cam);
    void SetViewMode(ViewMode mode, SE::CameraComponent& cam);
    void FocusOn(const DirectX::XMFLOAT3& center, float radius,
                 SE::CameraComponent& cam, float aspect);
    void Update(float dt, SE::InputManager& input, SE::CameraComponent& cam,
                HWND hwnd, bool viewportHovered, float viewportHeight);
    void CancelDrag();

    // View-plane motion and zoom are independent of platform input so their
    // framing/scale invariants can be regression tested.
    void Pan(float dx, float dy, float viewportHeight, SE::CameraComponent& cam);
    void Zoom(float amount, SE::CameraComponent& cam);
    bool IsFlying() const { return m_drag == DragMode::Fly; }
    bool IsNavigating() const { return m_drag != DragMode::None; }
    ViewMode GetViewMode() const { return m_viewMode; }

private:
    enum class DragMode { None, Fly, Walk, Orbit, Dolly, Pan };
    DragMode PickDrag(SE::InputManager& input, bool orthographic) const;
    DragMode PickSingleButtonDrag(SE::InputManager& input, bool orthographic) const;
    void UpdateDragMode(SE::InputManager& input, HWND hwnd, bool hovered, bool ortho);
    void SetDragMode(DragMode mode, HWND hwnd);
    void UpdateFly(float dt, SE::InputManager& input, HWND hwnd, SE::CameraComponent& cam);
    void MoveFly(float dt, SE::InputManager& input);
    void UpdateOrbit(float dx, float dy);
    void UpdateWalk(float dx, float dy);
    void WriteCamera(SE::CameraComponent& cam) const;
    DirectX::XMVECTOR Forward() const;
    DirectX::XMVECTOR Right() const;
    DirectX::XMVECTOR Up() const;

    DirectX::XMFLOAT3 m_eye = { 0.0f, 4.0f, -22.0f };
    float m_yawDeg = 0.0f;
    float m_pitchDeg = 0.0f;
    bool m_haveOrthoExtent = false;
    DragMode m_drag = DragMode::None;
    bool m_skipFirstWarp = false;
    POINT m_cursorBeforeFly = {};
    ViewMode m_viewMode = ViewMode::Perspective;
};

} // namespace Editor
