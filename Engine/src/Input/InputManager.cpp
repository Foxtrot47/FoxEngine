#include "Engine/Input/InputManager.h"
#include "Engine/Core/Logger.h"
#include <vector>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

namespace SE {

static InputManager* s_instance = nullptr;

bool InputManager::Init(HWND hwnd)
{
    s_instance = this;
    m_hwnd = hwnd;

    // Raw input for keyboard only — avoids conflict with ImGui's mouse raw input registration.
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage     = 0x06; // HID_USAGE_GENERIC_KEYBOARD
    rid.dwFlags     = 0;
    rid.hwndTarget  = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
    {
        SE_LOG_ERROR("RegisterRawInputDevices (keyboard) failed: 0x%08X", GetLastError());
        return false;
    }

    SE_LOG_INFO("InputManager initialised");
    return true;
}

static float NormalizeStick(int16_t value, int16_t deadZone)
{
    if (value >  deadZone) return static_cast<float>(value - deadZone)  / (32767.0f - deadZone);
    if (value < -deadZone) return static_cast<float>(value + deadZone)  / (32767.0f - deadZone);
    return 0.0f;
}

static float NormalizeTrigger(uint8_t value)
{
    if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return 0.0f;
    return static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
         / (255.0f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
}

void InputManager::PollGamepads()
{
    for (uint32_t i = 0; i < k_MaxGamepads; ++i)
    {
        XINPUT_STATE xs = {};
        bool connected  = (XInputGetState(i, &xs) == ERROR_SUCCESS);

        GamepadState& gs = m_gamepads[i];
        gs.connected = connected;

        if (!connected)
        {
            gs = GamepadState{};
            m_prevButtons[i] = 0;
            continue;
        }

        const XINPUT_GAMEPAD& xg = xs.Gamepad;

        uint16_t cur = xg.wButtons;
        uint16_t prev = m_prevButtons[i];
        gs.buttonsHeld     = cur;
        gs.buttonsPressed  = cur & ~prev;
        gs.buttonsReleased = prev & ~cur;
        m_prevButtons[i]   = cur;

        gs.leftX  = NormalizeStick(xg.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        gs.leftY  = NormalizeStick(xg.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        gs.rightX = NormalizeStick(xg.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        gs.rightY = NormalizeStick(xg.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        gs.leftTrigger  = NormalizeTrigger(xg.bLeftTrigger);
        gs.rightTrigger = NormalizeTrigger(xg.bRightTrigger);
    }
}

void InputManager::NewFrame()
{
    memset(m_keyPressed,  0, sizeof(m_keyPressed));
    memset(m_keyReleased, 0, sizeof(m_keyReleased));
    m_mouseDX    = 0;
    m_mouseDY    = 0;
    m_mouseWheel = 0;
    PollGamepads();

    if (m_cursorCaptured && !m_virtualCapture)
    {
        // While captured the delta comes from the cursor itself: queued
        // WM_MOUSEMOVEs straddle the re-centre warp and can't be trusted.
        const RECT r = CaptureClientRect();
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(m_hwnd, &cursor);
        m_mouseDX = cursor.x - (r.left + r.right) / 2;
        m_mouseDY = cursor.y - (r.top + r.bottom) / 2;
        RecentreCapturedCursor();
    }
}

RECT InputManager::CaptureClientRect() const
{
    if (m_captureRect.right > m_captureRect.left && m_captureRect.bottom > m_captureRect.top)
        return m_captureRect;
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    return client;
}

void InputManager::RecentreCapturedCursor()
{
    const RECT r = CaptureClientRect();
    POINT centre = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    IgnoreMouseMoveAt(centre.x, centre.y);
    ClientToScreen(m_hwnd, &centre);
    SetCursorPos(centre.x, centre.y);

    POINT topLeft = { r.left, r.top };
    POINT bottomRight = { r.right, r.bottom };
    ClientToScreen(m_hwnd, &topLeft);
    ClientToScreen(m_hwnd, &bottomRight);
    const RECT clip = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    ClipCursor(&clip);
}

void InputManager::SetCursorCaptured(bool captured)
{
    if (captured == m_cursorCaptured || !m_hwnd)
        return;
    if (m_virtualCapture)
    {
        m_cursorCaptured = captured;
        return;
    }
    m_cursorCaptured = captured;
    if (captured)
    {
        // ShowCursor keeps a display counter; one call per transition keeps it balanced.
        ShowCursor(FALSE);
        RecentreCapturedCursor();
    }
    else
    {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
    }
}

void InputManager::InjectKey(int vk, bool down)
{
    if (vk < 1 || vk > 255)
        return;
    if (down  && !m_keyDown[vk]) m_keyPressed[vk]  = true;
    if (!down &&  m_keyDown[vk]) m_keyReleased[vk] = true;
    m_keyDown[vk] = down;
}

void InputManager::InjectMouseDelta(int32_t dx, int32_t dy)
{
    m_mouseDX += dx;
    m_mouseDY += dy;
}

void InputManager::SetVirtualCapture(bool enabled)
{
    if (enabled == m_virtualCapture)
        return;
    m_virtualCapture = enabled;
    if (enabled)
    {
        if (m_cursorCaptured)
        {
            ClipCursor(nullptr);
            ShowCursor(TRUE);
        }
    }
    else
    {
        m_cursorCaptured = false;
    }
}

void InputManager::ReleaseAllKeys()
{
    // Key-up messages never arrive while unfocused, so report the release now
    // instead of leaving keys stuck down.
    for (int vk = 0; vk < 256; ++vk)
    {
        if (m_keyDown[vk])
        {
            m_keyDown[vk] = false;
            m_keyReleased[vk] = true;
        }
    }
}

bool InputManager::IsKeyDown    (int vk) const { return vk >= 0 && vk < 256 && m_keyDown[vk];     }
bool InputManager::IsKeyPressed (int vk) const { return vk >= 0 && vk < 256 && m_keyPressed[vk];  }
bool InputManager::IsKeyReleased(int vk) const { return vk >= 0 && vk < 256 && m_keyReleased[vk]; }

const GamepadState& InputManager::GetGamepad(uint32_t index) const
{
    static const GamepadState k_disconnected{};
    return (index < k_MaxGamepads) ? m_gamepads[index] : k_disconnected;
}

void InputManager::SetRumble(uint32_t index, float leftMotor, float rightMotor)
{
    if (index >= k_MaxGamepads || !m_gamepads[index].connected) return;
    XINPUT_VIBRATION vib;
    vib.wLeftMotorSpeed  = static_cast<WORD>(leftMotor  * 65535.0f);
    vib.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
    XInputSetState(index, &vib);
}

void InputManager::IgnoreMouseMoveAt(int32_t clientX, int32_t clientY)
{
    m_ignoreX       = clientX;
    m_ignoreY       = clientY;
    m_ignorePending = true;
}

LRESULT InputManager::WndProcHandler(HWND /*hwnd*/, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!s_instance) return 0;
    InputManager& im = *s_instance;

    auto setButton = [&](int vk, bool down)
    {
        if (down  && !im.m_keyDown[vk]) im.m_keyPressed[vk]  = true;
        if (!down &&  im.m_keyDown[vk]) im.m_keyReleased[vk] = true;
        im.m_keyDown[vk] = down;
    };

    switch (msg)
    {
    case WM_LBUTTONDOWN: setButton(VK_LBUTTON, true);  break;
    case WM_LBUTTONUP:   setButton(VK_LBUTTON, false); break;
    case WM_RBUTTONDOWN: setButton(VK_RBUTTON, true);  break;
    case WM_RBUTTONUP:   setButton(VK_RBUTTON, false); break;
    case WM_MBUTTONDOWN: setButton(VK_MBUTTON, true);  break;
    case WM_MBUTTONUP:   setButton(VK_MBUTTON, false); break;

    case WM_INPUT:
    {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                        nullptr, &size, sizeof(RAWINPUTHEADER));
        std::vector<BYTE> buf(size);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                            buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            break;

        const auto* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
        if (raw->header.dwType != RIM_TYPEKEYBOARD) break;

        const RAWKEYBOARD& kb = raw->data.keyboard;
        uint32_t vk = kb.VKey;
        if (vk == 0 || vk >= 256) break;

        bool down = !(kb.Flags & RI_KEY_BREAK);
        if (down  && !im.m_keyDown[vk]) im.m_keyPressed[vk]  = true;
        if (!down &&  im.m_keyDown[vk]) im.m_keyReleased[vk] = true;
        im.m_keyDown[vk] = down;
        break;
    }

    case WM_MOUSEMOVE:
    {
        int32_t x = static_cast<int32_t>(LOWORD(lp));
        int32_t y = static_cast<int32_t>(HIWORD(lp));
        // Captured deltas are sampled in NewFrame, and the synthetic move
        // produced by SetCursorPos re-centering is discarded: only track the position.
        const bool synthetic = im.m_ignorePending && x == im.m_ignoreX && y == im.m_ignoreY;
        if (im.m_cursorCaptured || synthetic)
        {
            if (synthetic) im.m_ignorePending = false;
            im.m_mouseAbsX     = x;
            im.m_mouseAbsY     = y;
            im.m_mouseHasPos   = true;
            break;
        }
        if (im.m_mouseHasPos)
        {
            im.m_mouseDX += x - im.m_mouseAbsX;
            im.m_mouseDY += y - im.m_mouseAbsY;
        }
        im.m_mouseAbsX   = x;
        im.m_mouseAbsY   = y;
        im.m_mouseHasPos = true;
        break;
    }

    case WM_MOUSEWHEEL:
        im.m_mouseWheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        break;

    case WM_ACTIVATEAPP:
        if (wp) break;
        [[fallthrough]];
    case WM_KILLFOCUS:
        im.SetCursorCaptured(false);
        im.ReleaseAllKeys();
        break;
    }

    return 0;
}

} // namespace SE
