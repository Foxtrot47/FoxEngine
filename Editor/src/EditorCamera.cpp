#include "EditorCamera.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace Editor {
namespace {
constexpr float k_minDistance = 0.05f;
constexpr float k_maxDistance = 100000.0f;

void WarpToCenter(HWND hwnd, SE::InputManager* input = nullptr)
{
    RECT r; GetClientRect(hwnd, &r);
    POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    if (input) input->IgnoreMouseMoveAt(c.x, c.y);
    ClientToScreen(hwnd, &c);
    SetCursorPos(c.x, c.y);
}

void PollWarpDelta(HWND hwnd, SE::InputManager& input, bool& skip, float& dx, float& dy)
{
    RECT r; GetClientRect(hwnd, &r);
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
    dx = skip ? 0.0f : static_cast<float>(p.x - (r.right - r.left) / 2);
    dy = skip ? 0.0f : static_cast<float>(p.y - (r.bottom - r.top) / 2);
    skip = false;
    WarpToCenter(hwnd, &input);
}

bool PressedMouse(SE::InputManager& input)
{
    return input.IsKeyPressed(VK_LBUTTON) || input.IsKeyPressed(VK_RBUTTON) ||
           input.IsKeyPressed(VK_MBUTTON);
}

float Axis(SE::InputManager& input, int positive, int negative)
{
    return static_cast<float>(input.IsKeyDown(positive)) - static_cast<float>(input.IsKeyDown(negative));
}
}

void EditorCamera::CancelDrag()
{
    if (m_drag == DragMode::Fly)
    {
        ShowCursor(TRUE);
        SetCursorPos(m_cursorBeforeFly.x, m_cursorBeforeFly.y);
    }
    m_drag = DragMode::None;
}

void EditorCamera::SyncFromCamera(const SE::CameraComponent& cam)
{
    CancelDrag();
    m_viewMode = ViewMode::Perspective;
    m_haveOrthoExtent = false;
    m_eye = cam.eye;
    const XMVECTOR delta = XMLoadFloat3(&cam.target) - XMLoadFloat3(&cam.eye);
    const float length = XMVectorGetX(XMVector3Length(delta));
    if (length < 1e-4f) return;
    const XMVECTOR dir = delta / length;
    m_yawDeg = XMConvertToDegrees(atan2f(XMVectorGetX(dir), XMVectorGetZ(dir)));
    m_pitchDeg = XMConvertToDegrees(asinf(std::clamp(XMVectorGetY(dir), -1.0f, 1.0f)));
    orbitDistance = (std::max)(length, k_minDistance);
}

void EditorCamera::SetViewMode(ViewMode mode, SE::CameraComponent& cam)
{
    CancelDrag();
    // Only initialize the ortho extent once. Changing view or returning from
    // perspective must not throw away the user's orthographic zoom.
    if (mode != ViewMode::Perspective && !m_haveOrthoExtent)
    {
        cam.orthoHeight = std::clamp(2.0f * orbitDistance *
            tanf(XMConvertToRadians(cam.fovDeg) * 0.5f), k_minDistance, k_maxDistance);
        m_haveOrthoExtent = true;
    }
    struct Angles { ViewMode mode; float yaw; float pitch; };
    static constexpr Angles presets[] = {
        { ViewMode::Isometric, 45.0f, -35.2643897f },
        { ViewMode::Top, 0.0f, -90.0f }, { ViewMode::Bottom, 0.0f, 90.0f },
        { ViewMode::Front, 0.0f, 0.0f }, { ViewMode::Back, 180.0f, 0.0f },
        { ViewMode::Left, 90.0f, 0.0f }, { ViewMode::Right, -90.0f, 0.0f }
    };
    for (const auto& preset : presets)
    {
        if (preset.mode != mode) continue;
        m_yawDeg = preset.yaw;
        m_pitchDeg = preset.pitch;
        break;
    }
    m_viewMode = mode;
    WriteCamera(cam);
}

void EditorCamera::FocusOn(const XMFLOAT3& center, float radius, SE::CameraComponent& cam, float aspect)
{
    radius = (std::max)(radius, 0.01f);
    aspect = (std::max)(aspect, 0.01f);
    const float halfFovY = XMConvertToRadians(cam.fovDeg) * 0.5f;
    const float halfFovX = atanf(tanf(halfFovY) * aspect);
    orbitDistance = radius * 1.1f / sinf((std::min)(halfFovX, halfFovY));
    orbitDistance = (std::max)(orbitDistance, radius + cam.nearZ);
    if (cam.orthographic)
    {
        cam.orthoHeight = 2.2f * radius / (std::min)(aspect, 1.0f);
        m_haveOrthoExtent = true;
    }
    XMStoreFloat3(&m_eye, XMLoadFloat3(&center) - Forward() * orbitDistance);
    WriteCamera(cam);
}

XMVECTOR EditorCamera::Forward() const
{
    const float yaw = XMConvertToRadians(m_yawDeg);
    const float pitch = XMConvertToRadians(m_pitchDeg);
    if (m_pitchDeg == 90.0f) return XMVectorSet(0, 1, 0, 0);
    if (m_pitchDeg == -90.0f) return XMVectorSet(0, -1, 0, 0);
    return XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch), 0);
}

XMVECTOR EditorCamera::Right() const
{
    const float yaw = XMConvertToRadians(m_yawDeg);
    return XMVectorSet(cosf(yaw), 0, -sinf(yaw), 0);
}

XMVECTOR EditorCamera::Up() const { return XMVector3Normalize(XMVector3Cross(Forward(), Right())); }

void EditorCamera::WriteCamera(SE::CameraComponent& cam) const
{
    cam.eye = m_eye;
    XMStoreFloat3(&cam.target, XMLoadFloat3(&m_eye) + Forward());
    XMStoreFloat3(&cam.up, Up()); // defined even when looking exactly along world Y
    cam.orthographic = m_viewMode != ViewMode::Perspective;
}

EditorCamera::DragMode EditorCamera::PickDrag(SE::InputManager& input, bool ortho) const
{
    const bool left = input.IsKeyDown(VK_LBUTTON);
    const bool right = input.IsKeyDown(VK_RBUTTON);
    if (left && right) return ortho ? DragMode::Dolly : DragMode::Pan;
    if (input.IsKeyDown(VK_MBUTTON)) return DragMode::Pan;
    return PickSingleButtonDrag(input, ortho);
}

EditorCamera::DragMode EditorCamera::PickSingleButtonDrag(SE::InputManager& input, bool ortho) const
{
    const bool left = input.IsKeyDown(VK_LBUTTON);
    const bool right = input.IsKeyDown(VK_RBUTTON);
    if (input.IsKeyDown(VK_MENU))
    {
        if (left) return DragMode::Orbit;
        if (right) return DragMode::Dolly;
    }
    if (right) return ortho ? DragMode::Pan : DragMode::Fly;
    if (left && !ortho) return DragMode::Walk;
    return DragMode::None;
}

void EditorCamera::SetDragMode(DragMode mode, HWND hwnd)
{
    if (mode == m_drag) return;
    CancelDrag();
    m_drag = mode;
    if (mode != DragMode::Fly) return;
    GetCursorPos(&m_cursorBeforeFly);
    ShowCursor(FALSE);
    m_skipFirstWarp = true;
    WarpToCenter(hwnd);
}

void EditorCamera::UpdateDragMode(SE::InputManager& input, HWND hwnd, bool hovered, bool ortho)
{
    if (GetForegroundWindow() != hwnd)
    {
        CancelDrag();
        return;
    }
    if (!IsNavigating() && !(hovered && PressedMouse(input))) return;
    // Re-evaluate chords during a drag; adding/removing LMB from RMB must
    // switch pan/zoom immediately, not wait for every button to be released.
    const DragMode next = PickDrag(input, ortho);
    const bool leavingFly = m_drag == DragMode::Fly && next != DragMode::Fly;
    SetDragMode(next, hwnd);
    if (leavingFly)
    {
        POINT cursor; GetCursorPos(&cursor); ScreenToClient(hwnd, &cursor);
        input.IgnoreMouseMoveAt(cursor.x, cursor.y);
    }
}

void EditorCamera::Update(float dt, SE::InputManager& input, SE::CameraComponent& cam,
                          HWND hwnd, bool hovered, float viewportHeight)
{
    const DragMode previous = m_drag;
    UpdateDragMode(input, hwnd, hovered, cam.orthographic);
    // Movement accumulated before a press belongs to cursor positioning, not
    // navigation. Also discard the transition frame when changing mouse chords.
    const float dx = previous == m_drag ? static_cast<float>(input.GetMouseDeltaX()) : 0.0f;
    const float dy = previous == m_drag ? static_cast<float>(input.GetMouseDeltaY()) : 0.0f;
    switch (m_drag)
    {
    case DragMode::Fly: UpdateFly(dt, input, hwnd, cam); break;
    case DragMode::Walk: UpdateWalk(dx, dy); break;
    case DragMode::Orbit: UpdateOrbit(dx, dy); break;
    case DragMode::Dolly: Zoom((dx - dy) * 0.03f, cam); break;
    case DragMode::Pan: Pan(dx, dy, viewportHeight, cam); break;
    case DragMode::None:
        if (hovered) Zoom(static_cast<float>(input.GetMouseWheelDelta()), cam);
        break;
    }
    WriteCamera(cam);
}

void EditorCamera::MoveFly(float dt, SE::InputManager& input)
{
    float forward = Axis(input, 'W', 'S') + Axis(input, VK_UP, VK_DOWN) + Axis(input, VK_NUMPAD8, VK_NUMPAD2);
    float right = Axis(input, 'D', 'A') + Axis(input, VK_RIGHT, VK_LEFT) + Axis(input, VK_NUMPAD6, VK_NUMPAD4);
    float up = Axis(input, 'E', 'Q') + Axis(input, VK_PRIOR, VK_NEXT) + Axis(input, VK_NUMPAD9, VK_NUMPAD7);
    const XMVECTOR movement = Forward() * std::clamp(forward, -1.0f, 1.0f) +
        Right() * std::clamp(right, -1.0f, 1.0f) + XMVectorSet(0, 1, 0, 0) * std::clamp(up, -1.0f, 1.0f) +
        Up() * Axis(input, 'R', 'F');
    const float speed = moveSpeed * (input.IsKeyDown(VK_SHIFT) ? 3.0f : 1.0f);
    XMStoreFloat3(&m_eye, XMLoadFloat3(&m_eye) + movement * speed * dt);
}

void EditorCamera::UpdateFly(float dt, SE::InputManager& input, HWND hwnd, SE::CameraComponent& cam)
{
    float dx, dy;
    PollWarpDelta(hwnd, input, m_skipFirstWarp, dx, dy);
    m_yawDeg += dx * sensitivity;
    m_pitchDeg = std::clamp(m_pitchDeg - dy * sensitivity, -89.0f, 89.0f);
    moveSpeed = std::clamp(moveSpeed * powf(1.2f, static_cast<float>(input.GetMouseWheelDelta())), 0.2f, 400.0f);
    cam.fovDeg = std::clamp(cam.fovDeg + Axis(input, 'Z', 'C') * dt * 30.0f, 20.0f, 120.0f);
    MoveFly(dt, input);
}

void EditorCamera::UpdateOrbit(float dx, float dy)
{
    if (dx == 0 && dy == 0) return;
    const XMVECTOR pivot = XMLoadFloat3(&m_eye) + Forward() * orbitDistance;
    m_yawDeg += dx * sensitivity;
    m_pitchDeg = std::clamp(m_pitchDeg - dy * sensitivity, -89.0f, 89.0f);
    XMStoreFloat3(&m_eye, pivot - Forward() * orbitDistance);
    if (m_viewMode != ViewMode::Perspective) m_viewMode = ViewMode::Orthographic;
}

void EditorCamera::UpdateWalk(float dx, float dy)
{
    m_yawDeg += dx * sensitivity;
    const float yaw = XMConvertToRadians(m_yawDeg);
    const XMVECTOR planarForward = XMVectorSet(sinf(yaw), 0, cosf(yaw), 0);
    XMStoreFloat3(&m_eye, XMLoadFloat3(&m_eye) - planarForward * dy * moveSpeed * 0.01f);
}

void EditorCamera::Pan(float dx, float dy, float viewportHeight, SE::CameraComponent& cam)
{
    const float extent = cam.orthographic ? cam.orthoHeight :
        2.0f * orbitDistance * tanf(XMConvertToRadians(cam.fovDeg) * 0.5f);
    const float scale = extent / (std::max)(viewportHeight, 1.0f);
    XMStoreFloat3(&m_eye, XMLoadFloat3(&m_eye) - Right() * dx * scale + Up() * dy * scale);
    WriteCamera(cam);
}

void EditorCamera::Zoom(float amount, SE::CameraComponent& cam)
{
    if (amount == 0.0f) return;
    const float factor = powf(1.2f, -std::clamp(amount, -50.0f, 50.0f));
    if (cam.orthographic)
    {
        cam.orthoHeight = std::clamp(cam.orthoHeight * factor, k_minDistance, k_maxDistance);
        m_haveOrthoExtent = true;
        return;
    }
    const float previous = orbitDistance;
    orbitDistance = std::clamp(previous * factor, k_minDistance, k_maxDistance);
    XMStoreFloat3(&m_eye, XMLoadFloat3(&m_eye) + Forward() * (previous - orbitDistance));
    WriteCamera(cam);
}
} // namespace Editor
