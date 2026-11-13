#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SE {

// One named input source: a Win32 virtual-key code (keyboard keys and mouse
// buttons, as InputManager reports them) or an XInput gamepad button mask.
struct KeyCode
{
    enum class Device : uint8_t { Keyboard, Gamepad };
    Device   device = Device::Keyboard;
    uint16_t code   = 0;
};

// Parses a key name such as "W", "Space", "MouseRight" or "Gamepad.Start".
// Matching ignores case; AllKeyNames() lists the canonical spellings.
bool ParseKeyName(std::string_view name, KeyCode& out);

// Every canonical key name in a stable order (docs, validation, stubs).
const std::vector<std::string>& AllKeyNames();

} // namespace SE
