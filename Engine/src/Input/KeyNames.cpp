#include "Engine/Input/KeyNames.h"
#include <windows.h>
#include "Engine/Input/GamepadState.h"
#include <cctype>
#include <unordered_map>
#include <utility>

namespace SE {

namespace {

struct KeyTable
{
    std::vector<std::string> names;
    std::unordered_map<std::string, KeyCode> byLowerName;

    void Add(std::string name, KeyCode::Device device, uint16_t code)
    {
        std::string lower = name;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        byLowerName.emplace(std::move(lower), KeyCode{ device, code });
        names.push_back(std::move(name));
    }
    void Key(std::string name, int vk) { Add(std::move(name), KeyCode::Device::Keyboard, static_cast<uint16_t>(vk)); }
    void Pad(std::string name, uint16_t mask) { Add("Gamepad." + std::move(name), KeyCode::Device::Gamepad, mask); }
};

const KeyTable& Table()
{
    static const KeyTable table = [] {
        KeyTable t;
        for (char c = 'A'; c <= 'Z'; ++c) t.Key(std::string(1, c), c);
        for (char c = '0'; c <= '9'; ++c) t.Key(std::string(1, c), c);
        for (int i = 1; i <= 12; ++i) t.Key("F" + std::to_string(i), VK_F1 + i - 1);

        t.Key("Space", VK_SPACE);
        t.Key("Enter", VK_RETURN);
        t.Key("Escape", VK_ESCAPE);
        t.Key("Tab", VK_TAB);
        t.Key("Backspace", VK_BACK);
        t.Key("Insert", VK_INSERT);
        t.Key("Delete", VK_DELETE);
        t.Key("Home", VK_HOME);
        t.Key("End", VK_END);
        t.Key("PageUp", VK_PRIOR);
        t.Key("PageDown", VK_NEXT);
        t.Key("Up", VK_UP);
        t.Key("Down", VK_DOWN);
        t.Key("Left", VK_LEFT);
        t.Key("Right", VK_RIGHT);
        // Raw keyboard input reports the generic modifier codes for either side.
        t.Key("Shift", VK_SHIFT);
        t.Key("Ctrl", VK_CONTROL);
        t.Key("Alt", VK_MENU);
        t.Key("CapsLock", VK_CAPITAL);

        t.Key("Backquote", VK_OEM_3);
        t.Key("Minus", VK_OEM_MINUS);
        t.Key("Equals", VK_OEM_PLUS);
        t.Key("LeftBracket", VK_OEM_4);
        t.Key("RightBracket", VK_OEM_6);
        t.Key("Backslash", VK_OEM_5);
        t.Key("Semicolon", VK_OEM_1);
        t.Key("Quote", VK_OEM_7);
        t.Key("Comma", VK_OEM_COMMA);
        t.Key("Period", VK_OEM_PERIOD);
        t.Key("Slash", VK_OEM_2);

        for (int i = 0; i <= 9; ++i) t.Key("Numpad" + std::to_string(i), VK_NUMPAD0 + i);
        t.Key("NumpadAdd", VK_ADD);
        t.Key("NumpadSubtract", VK_SUBTRACT);
        t.Key("NumpadMultiply", VK_MULTIPLY);
        t.Key("NumpadDivide", VK_DIVIDE);
        t.Key("NumpadDecimal", VK_DECIMAL);

        t.Key("MouseLeft", VK_LBUTTON);
        t.Key("MouseRight", VK_RBUTTON);
        t.Key("MouseMiddle", VK_MBUTTON);

        t.Pad("A", GamepadButton::A);
        t.Pad("B", GamepadButton::B);
        t.Pad("X", GamepadButton::X);
        t.Pad("Y", GamepadButton::Y);
        t.Pad("LeftShoulder", GamepadButton::LeftShoulder);
        t.Pad("RightShoulder", GamepadButton::RightShoulder);
        t.Pad("LeftThumb", GamepadButton::LeftThumb);
        t.Pad("RightThumb", GamepadButton::RightThumb);
        t.Pad("Start", GamepadButton::Start);
        t.Pad("Back", GamepadButton::Back);
        t.Pad("DpadUp", GamepadButton::DpadUp);
        t.Pad("DpadDown", GamepadButton::DpadDown);
        t.Pad("DpadLeft", GamepadButton::DpadLeft);
        t.Pad("DpadRight", GamepadButton::DpadRight);
        return t;
    }();
    return table;
}

} // namespace

bool ParseKeyName(std::string_view name, KeyCode& out)
{
    std::string lower(name);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto& table = Table().byLowerName;
    const auto found = table.find(lower);
    if (found == table.end())
        return false;
    out = found->second;
    return true;
}

const std::vector<std::string>& AllKeyNames()
{
    return Table().names;
}

} // namespace SE
