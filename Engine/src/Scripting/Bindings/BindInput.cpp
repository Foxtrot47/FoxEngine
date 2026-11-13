#include <algorithm>
#include <string>
#include <tuple>
#include "Engine/Input/InputManager.h"
#include "Engine/Input/KeyNames.h"
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"

namespace SE::Scripting {

namespace {

KeyCode ParseKey(const std::string& name)
{
    KeyCode code;
    if (!ParseKeyName(name, code))
        Raise("unknown key '%s' (see fox.Key in Engine/Scripts/types/fox.lua)", name.c_str());
    return code;
}

uint16_t ParseGamepadButton(const std::string& name)
{
    KeyCode code;
    const bool qualified = name.rfind("Gamepad.", 0) == 0;
    if (!ParseKeyName(qualified ? name : "Gamepad." + name, code) || code.device != KeyCode::Device::Gamepad)
        Raise("unknown gamepad button '%s' (see fox.GamepadButton)", name.c_str());
    return code.code;
}

uint32_t PadIndex(sol::optional<lua_Integer> index)
{
    const lua_Integer value = index.value_or(0);
    if (value < 0 || value >= static_cast<lua_Integer>(k_MaxGamepads))
        Raise("gamepad index %lld is out of range 0..%u", static_cast<long long>(value), k_MaxGamepads - 1);
    return static_cast<uint32_t>(value);
}

// Input is neutral without focus (editor not playing in the viewport, player
// window inactive) and in headless sessions.
const InputManager* FocusedInput(const ScriptContext& ctx)
{
    return ctx.inputFocus ? ctx.host.input : nullptr;
}

enum class Edge { Down, Pressed, Released };

bool KeyState(const ScriptContext& ctx, const std::string& name, Edge edge)
{
    const KeyCode key = ParseKey(name);
    const InputManager* input = FocusedInput(ctx);
    if (!input)
        return false;
    if (key.device == KeyCode::Device::Gamepad)
    {
        for (uint32_t pad = 0; pad < k_MaxGamepads; ++pad)
        {
            const GamepadState& state = input->GetGamepad(pad);
            const bool hit = edge == Edge::Down    ? state.IsButtonDown(key.code)
                           : edge == Edge::Pressed ? state.IsButtonPressed(key.code)
                                                   : state.IsButtonReleased(key.code);
            if (hit)
                return true;
        }
        return false;
    }
    return edge == Edge::Down    ? input->IsKeyDown(key.code)
         : edge == Edge::Pressed ? input->IsKeyPressed(key.code)
                                 : input->IsKeyReleased(key.code);
}

bool ActionState(const ScriptContext& ctx, const std::string& action, Edge edge)
{
    if (!ctx.actionNames.count(action))
        Raise("unknown input action '%s' (actions are declared in project.json \"input\")", action.c_str());
    if (!FocusedInput(ctx))
        return false;
    return edge == Edge::Down    ? ctx.actions.IsHeld(action)
         : edge == Edge::Pressed ? ctx.actions.IsPressed(action)
                                 : ctx.actions.IsReleased(action);
}

} // namespace

void BindInput(ScriptApi& api, ScriptContext& ctx)
{
    std::vector<std::string> keys;
    std::vector<std::string> buttons;
    for (const std::string& name : AllKeyNames())
    {
        keys.push_back("\"" + name + "\"");
        if (name.rfind("Gamepad.", 0) == 0)
            buttons.push_back("\"" + name.substr(8) + "\"");
    }
    api.DocAlias("fox.Key",
        "Key, mouse button or gamepad button name (matching ignores case).\n"
        "Gamepad.* names match any connected pad.", std::move(keys));
    api.DocAlias("fox.GamepadButton", "Gamepad button name for fox.input.gamepad_down/pressed.",
                 std::move(buttons));

    ApiDocs::Block& pad = api.DocClass("fox.Gamepad", "Snapshot returned by fox.input.gamepad.");
    pad.fields = {
        { "connected", "boolean", "" },
        { "left_x", "number", "Left stick, -1..1 with dead zone." },
        { "left_y", "number", "" },
        { "right_x", "number", "" },
        { "right_y", "number", "" },
        { "left_trigger", "number", "0..1" },
        { "right_trigger", "number", "0..1" },
    };

    api.Module("input",
        "Keyboard, mouse and gamepad state for this frame. Everything reads as\n"
        "neutral (nothing held, zero deltas) while the game has no input focus.")
        .Function("key_down", "(key: fox.Key) -> boolean", "True while the key is held.",
                  [&ctx](const std::string& key) { return KeyState(ctx, key, Edge::Down); })
        .Function("key_pressed", "(key: fox.Key) -> boolean", "True on the frame the key went down.",
                  [&ctx](const std::string& key) { return KeyState(ctx, key, Edge::Pressed); })
        .Function("key_released", "(key: fox.Key) -> boolean", "True on the frame the key went up.",
                  [&ctx](const std::string& key) { return KeyState(ctx, key, Edge::Released); })
        .Function("action_down", "(action: string) -> boolean",
                  "True while any key bound to the project.json action is held.",
                  [&ctx](const std::string& action) { return ActionState(ctx, action, Edge::Down); })
        .Function("action_pressed", "(action: string) -> boolean", "True on the frame the action started.",
                  [&ctx](const std::string& action) { return ActionState(ctx, action, Edge::Pressed); })
        .Function("action_released", "(action: string) -> boolean", "True on the frame the action ended.",
                  [&ctx](const std::string& action) { return ActionState(ctx, action, Edge::Released); })
        .Function("mouse_delta", "() -> number, number", "Mouse movement in pixels this frame (x right, y down).",
                  [&ctx] {
                      const InputManager* input = FocusedInput(ctx);
                      if (!input)
                          return std::make_tuple(0.0, 0.0);
                      return std::make_tuple(static_cast<double>(input->GetMouseDeltaX()),
                                             static_cast<double>(input->GetMouseDeltaY()));
                  })
        .Function("mouse_position", "() -> number, number", "Cursor position relative to the game view.",
                  [&ctx] {
                      if (!ctx.host.input)
                          return std::make_tuple(0.0, 0.0);
                      return std::make_tuple(ctx.host.input->GetMouseX() - static_cast<double>(ctx.view.x),
                                             ctx.host.input->GetMouseY() - static_cast<double>(ctx.view.y));
                  })
        .Function("mouse_wheel", "() -> number", "Wheel notches this frame (positive = away from the user).",
                  [&ctx] {
                      const InputManager* input = FocusedInput(ctx);
                      return input ? static_cast<double>(input->GetMouseWheelDelta()) : 0.0;
                  })
        .Function("gamepad", "(index?: integer) -> fox.Gamepad", "Stick and trigger state of pad 0..3 (default 0).",
                  [&ctx](sol::optional<lua_Integer> index, sol::this_state s) {
                      const uint32_t padIndex = PadIndex(index);
                      sol::state_view lua(s);
                      sol::table result = lua.create_table();
                      const InputManager* input = FocusedInput(ctx);
                      const GamepadState empty;
                      const GamepadState& state = input ? input->GetGamepad(padIndex) : empty;
                      result["connected"] = ctx.host.input && ctx.host.input->GetGamepad(padIndex).connected;
                      result["left_x"] = state.leftX;
                      result["left_y"] = state.leftY;
                      result["right_x"] = state.rightX;
                      result["right_y"] = state.rightY;
                      result["left_trigger"] = state.leftTrigger;
                      result["right_trigger"] = state.rightTrigger;
                      return result;
                  })
        .Function("gamepad_down", "(button: fox.GamepadButton, index?: integer) -> boolean",
                  "True while the button is held on pad index (default 0).",
                  [&ctx](const std::string& button, sol::optional<lua_Integer> index) {
                      const uint16_t mask = ParseGamepadButton(button);
                      const InputManager* input = FocusedInput(ctx);
                      return input && input->GetGamepad(PadIndex(index)).IsButtonDown(mask);
                  })
        .Function("gamepad_pressed", "(button: fox.GamepadButton, index?: integer) -> boolean",
                  "True on the frame the button went down on pad index (default 0).",
                  [&ctx](const std::string& button, sol::optional<lua_Integer> index) {
                      const uint16_t mask = ParseGamepadButton(button);
                      const InputManager* input = FocusedInput(ctx);
                      return input && input->GetGamepad(PadIndex(index)).IsButtonPressed(mask);
                  })
        .Function("set_rumble", "(index: integer, left: number, right: number) -> nil",
                  "Sets the pad's motor speeds (0..1).",
                  [&ctx](lua_Integer index, double left, double right) {
                      const uint32_t padIndex = PadIndex(index);
                      if (ctx.host.input)
                          ctx.host.input->SetRumble(padIndex, static_cast<float>(std::clamp(left, 0.0, 1.0)),
                                                    static_cast<float>(std::clamp(right, 0.0, 1.0)));
                  })
        .Function("capture_cursor", "(capture: boolean) -> nil",
                  "Hides and locks the cursor for mouse-look (mouse_delta keeps working).\n"
                  "Ignored without input focus; released automatically when focus is lost.",
                  [&ctx](bool capture) {
                      if (!ctx.host.input || (capture && !ctx.inputFocus))
                          return;
                      ctx.host.input->SetCursorCaptured(capture);
                  })
        .Function("cursor_captured", "() -> boolean", "True while the cursor is captured.",
                  [&ctx] { return ctx.host.input && ctx.host.input->IsCursorCaptured(); });
}

} // namespace SE::Scripting
