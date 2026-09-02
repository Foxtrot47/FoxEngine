#include "EditorCamera.h"

#include "Engine/Input/InputManager.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Editor {

namespace {

constexpr float k_minSpeed = 0.2f;
constexpr float k_maxSpeed = 400.0f;
constexpr float k_minOrbitDistance = 0.5f;

void BeginMouseWarp(HWND hwnd, bool& skipFirst)
{
    skipFirst = true;
    ShowCursor(FALSE);
    RECT r; GetClientRect(hwnd, &r);
    POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    ClientToScreen(hwnd, &c);
    SetCursorPos(c.x, c.y);
}

void EndMouseWarp()
{
    ShowCursor(TRUE);
}

// Reads mouse delta via the screen-centre warp technique so the cursor never
// hits a screen edge during a fly.
void PollWarpDelta(HWND hwnd, SE::InputManager& input,
                   bool& skipFirst, float& outDX, float& outDY)
{
    RECT r; GetClientRect(hwnd, &r);
    int32_t cx = (r.right  - r.left) / 2;
    int32_t cy = (r.bottom - r.top)  / 2;

    outDX = outDY = 0.0f;
    if (!skipFirst)
    {
        POINT cursor; GetCursorPos(&cursor); ScreenToClient(hwnd, &cursor);
        outDX = static_cast<float>(cursor.x - cx);
        outDY = static_cast<float>(cursor.y - cy);
    }
    skipFirst = false;

    input.IgnoreMouseMoveAt(cx, cy);
    POINT screen = { cx, cy }; ClientToScreen(hwnd, &screen);
    SetCursorPos(screen.x, screen.y);
}

} // namespace

void EditorCamera::SyncFromCamera(const SE::CameraComponent& cam)
{
    m_eye = cam.eye;
    XMVECTOR toTarget = XMVectorSubtract(XMLoadFloat3(&cam.target), XMLoadFloat3(&cam.eye));
    const float len = XMVectorGetX(XMVector3Length(toTarget));
    if (len > 1e-4f)
    {
        XMVECTOR dir = XMVectorScale(toTarget, 1.0f / len);
        m_yawDeg   = XMConvertToDegrees(atan2f(XMVectorGetX(dir), XMVectorGetZ(dir)));
        m_pitchDeg = XMConvertToDegrees(asinf(std::clamp(XMVectorGetY(dir), -1.0f, 1.0f)));
        orbitDistance = (std::max)(len, k_minOrbitDistance);
    }
}

XMVECTOR EditorCamera::Forward() const
{
    const float yaw   = XMConvertToRadians(m_yawDeg);
    const float pitch = XMConvertToRadians(m_pitchDeg);
    return XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch), 0.0f);
}

XMVECTOR EditorCamera::Right() const
{
    const float yaw = XMConvertToRadians(m_yawDeg);
    return XMVectorSet(cosf(yaw), 0.0f, -sinf(yaw), 0.0f);
}

void EditorCamera::Update(float dt, SE::InputManager& input, SE::CameraComponent& cam,
                          HWND hwnd, bool viewportHovered)
{
    UpdateDragMode(input, hwnd, viewportHovered);

    switch (m_drag)
    {
    case DragMode::Fly:   UpdateFly(dt, input, hwnd); break;
    case DragMode::Orbit: UpdateOrbit(input);         break;
    case DragMode::Dolly: UpdateDolly(input);         break;
    case DragMode::Pan:   UpdatePan(input);           break;
    case DragMode::None:  UpdateWheelDolly(input, viewportHovered); break;
    }

    XMVECTOR fwd = Forward();
    cam.eye = m_eye;
    XMStoreFloat3(&cam.target, XMVectorAdd(XMLoadFloat3(&m_eye), fwd));
    cam.up = { 0.0f, 1.0f, 0.0f };
}

bool EditorCamera::DragButtonHeld(SE::InputManager& input) const
{
    switch (m_drag)
    {
    case DragMode::Fly:
    case DragMode::Dolly: return input.IsKeyDown(VK_RBUTTON);
    case DragMode::Orbit: return input.IsKeyDown(VK_LBUTTON);
    case DragMode::Pan:   return input.IsKeyDown(VK_MBUTTON);
    default:              return false;
    }
}

EditorCamera::DragMode EditorCamera::PickDragStart(SE::InputManager& input)
{
    const bool alt = input.IsKeyDown(VK_MENU);
    if (input.IsKeyDown(VK_RBUTTON))       return alt ? DragMode::Dolly : DragMode::Fly;
    if (alt && input.IsKeyDown(VK_LBUTTON)) return DragMode::Orbit;
    if (input.IsKeyDown(VK_MBUTTON))        return DragMode::Pan;
    return DragMode::None;
}

void EditorCamera::UpdateDragMode(SE::InputManager& input, HWND hwnd, bool viewportHovered)
{
    // End the active drag when its button is released.
    if (m_drag != DragMode::None && !DragButtonHeld(input))
    {
        if (m_drag == DragMode::Fly)
            EndMouseWarp();
        m_drag = DragMode::None;
    }

    // Start a new drag only over the viewport.
    if (m_drag != DragMode::None || !viewportHovered)
        return;
    m_drag = PickDragStart(input);
    if (m_drag == DragMode::Fly)
        BeginMouseWarp(hwnd, m_skipFirstWarp);
}

void EditorCamera::UpdateFly(float dt, SE::InputManager& input, HWND hwnd)
{
    float dx, dy;
    PollWarpDelta(hwnd, input, m_skipFirstWarp, dx, dy);
    m_yawDeg   += dx * sensitivity;
    m_pitchDeg -= dy * sensitivity;
    m_pitchDeg  = std::clamp(m_pitchDeg, -89.0f, 89.0f);

    // Scroll while flying adjusts the fly speed (UE behaviour).
    const int wheel = input.GetMouseWheelDelta();
    if (wheel != 0)
        moveSpeed = std::clamp(moveSpeed * powf(1.2f, (float)wheel), k_minSpeed, k_maxSpeed);

    float speed = moveSpeed;
    if (input.IsKeyDown(VK_SHIFT)) speed *= 3.0f;

    XMVECTOR eye = XMLoadFloat3(&m_eye);
    XMVECTOR fwd = Forward();
    XMVECTOR rgt = Right();
    XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    if (input.IsKeyDown('W')) eye = XMVectorAdd(eye, XMVectorScale(fwd, speed * dt));
    if (input.IsKeyDown('S')) eye = XMVectorSubtract(eye, XMVectorScale(fwd, speed * dt));
    if (input.IsKeyDown('D')) eye = XMVectorAdd(eye, XMVectorScale(rgt, speed * dt));
    if (input.IsKeyDown('A')) eye = XMVectorSubtract(eye, XMVectorScale(rgt, speed * dt));
    if (input.IsKeyDown('E')) eye = XMVectorAdd(eye, XMVectorScale(up, speed * dt));
    if (input.IsKeyDown('Q')) eye = XMVectorSubtract(eye, XMVectorScale(up, speed * dt));
    XMStoreFloat3(&m_eye, eye);
}

void EditorCamera::UpdateOrbit(SE::InputManager& input)
{
    const float dx = static_cast<float>(input.GetMouseDeltaX());
    const float dy = static_cast<float>(input.GetMouseDeltaY());

    XMVECTOR pivot = XMVectorAdd(XMLoadFloat3(&m_eye), XMVectorScale(Forward(), orbitDistance));
    m_yawDeg   += dx * sensitivity;
    m_pitchDeg -= dy * sensitivity;
    m_pitchDeg  = std::clamp(m_pitchDeg, -89.0f, 89.0f);
    XMStoreFloat3(&m_eye, XMVectorSubtract(pivot, XMVectorScale(Forward(), orbitDistance)));
}

void EditorCamera::UpdateDolly(SE::InputManager& input)
{
    const float dx = static_cast<float>(input.GetMouseDeltaX());
    const float dy = static_cast<float>(input.GetMouseDeltaY());

    XMVECTOR pivot = XMVectorAdd(XMLoadFloat3(&m_eye), XMVectorScale(Forward(), orbitDistance));
    // Drag up / right zooms in (UE behaviour); step scales with distance.
    const float step = (dx - dy) * orbitDistance * 0.005f;
    orbitDistance = (std::max)(orbitDistance - step, k_minOrbitDistance);
    XMStoreFloat3(&m_eye, XMVectorSubtract(pivot, XMVectorScale(Forward(), orbitDistance)));
}

void EditorCamera::UpdatePan(SE::InputManager& input)
{
    const float dx = static_cast<float>(input.GetMouseDeltaX());
    const float dy = static_cast<float>(input.GetMouseDeltaY());

    // Content follows the cursor: drag right moves the camera left, drag down
    // moves the camera up. Pan speed scales with the focus distance.
    const float scale = orbitDistance * 0.002f;
    XMVECTOR eye = XMLoadFloat3(&m_eye);
    XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    eye = XMVectorSubtract(eye, XMVectorScale(Right(), dx * scale));
    eye = XMVectorAdd(eye, XMVectorScale(up, dy * scale));
    XMStoreFloat3(&m_eye, eye);
}

void EditorCamera::UpdateWheelDolly(SE::InputManager& input, bool viewportHovered)
{
    const int wheel = input.GetMouseWheelDelta();
    if (wheel == 0 || !viewportHovered)
        return;

    // Dolly toward the focus point; steps shrink as the camera approaches it.
    const float step = (std::max)(orbitDistance * 0.15f, 0.3f) * (float)wheel;
    orbitDistance = (std::max)(orbitDistance - step, k_minOrbitDistance);
    XMStoreFloat3(&m_eye, XMVectorAdd(XMLoadFloat3(&m_eye), XMVectorScale(Forward(), step)));
}

} // namespace Editor
