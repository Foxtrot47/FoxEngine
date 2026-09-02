#pragma once
#include <windows.h>
#include <DirectXMath.h>

namespace SE {
class InputManager;
struct CameraComponent;
}

namespace Editor {

// Unreal-style viewport camera:
//   RMB hold      — fly: mouse look, WASD move, Q down / E up,
//                   scroll wheel adjusts fly speed
//   Alt + LMB     — orbit around the focus point
//   Alt + RMB     — dolly toward / away from the focus point
//   MMB drag      — pan (track)
//   Scroll wheel  — dolly forward / back
class EditorCamera
{
public:
    float moveSpeed     = 20.0f;   // fly units/sec
    float sensitivity   = 0.15f;   // look deg/pixel
    float orbitDistance = 10.0f;   // distance to the orbit/dolly focus point

    // Derive yaw/pitch/eye from a scene camera (call when a scene finishes loading).
    void SyncFromCamera(const SE::CameraComponent& cam);

    // Frame a target (UE 'F'): keep the view direction, move the camera to a
    // bounds-proportional distance, and make the target the orbit pivot.
    void FocusOn(const DirectX::XMFLOAT3& center, float radius);

    // Drives cam.eye/target/up. viewportHovered gates the start of any drag;
    // an active drag continues until its mouse button is released.
    void Update(float dt, SE::InputManager& input, SE::CameraComponent& cam,
                HWND hwnd, bool viewportHovered);

    bool IsFlying() const { return m_drag == DragMode::Fly; }

private:
    enum class DragMode { None, Fly, Orbit, Dolly, Pan };

    bool DragButtonHeld(SE::InputManager& input) const;
    DragMode PickDragStart(SE::InputManager& input);
    void UpdateDragMode(SE::InputManager& input, HWND hwnd, bool viewportHovered);
    void UpdateFly(float dt, SE::InputManager& input, HWND hwnd);
    void UpdateOrbit(SE::InputManager& input);
    void UpdateDolly(SE::InputManager& input);
    void UpdatePan(SE::InputManager& input);
    void UpdateWheelDolly(SE::InputManager& input, bool viewportHovered);

    DirectX::XMVECTOR Forward() const;
    DirectX::XMVECTOR Right() const;

    DirectX::XMFLOAT3 m_eye      = { 0.0f, 4.0f, -22.0f };
    float             m_yawDeg   = 0.0f;
    float             m_pitchDeg = 0.0f;

    DragMode m_drag           = DragMode::None;
    bool     m_skipFirstWarp  = false;
};

} // namespace Editor
