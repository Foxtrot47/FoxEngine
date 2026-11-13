#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Engine/Scene/ActorTypes.h"

namespace SE {

class InputManager;
class SceneRuntime;
class Window;

// Engine services a script session may use. Everything is optional: headless
// tools and tests run with no window, input or scene, and the corresponding
// fox.* calls raise "not available" errors.
struct ScriptHost
{
    InputManager* input  = nullptr;
    Window*       window = nullptr;
    // The scene scripts run in: fox.scene, fox.camera, fox.physics,
    // fox.graphics and entity scripts. Simulate drives its Update.
    SceneRuntime* scene  = nullptr;

    std::string projectName;
    bool editor    = false;   // fox.app.is_editor()
    bool published = false;   // fox.app.is_published()
    bool hotReload = false;   // poll loaded files and patch changed modules

    // Input actions for fox.input.action_*: action name -> key names
    // ("W", "MouseRight", "Gamepad.Start").
    std::vector<std::pair<std::string, std::vector<std::string>>> inputActions;

    // fox.scene.list(): the project's scenes (Project::SceneList).
    std::vector<std::string> scenes;

    // Additional require() roots searched after Assets/Scripts/ (tests add
    // their fixture directory).
    std::vector<std::string> extraSearchRoots;

    std::function<void()> requestQuit;                      // fox.app.quit()
    std::function<void(const std::string&)> setTitle;       // fox.window.set_title()
};

// The game view in ImGui/client pixels: the whole client area in the player,
// the viewport image in the editor. fox.ui and mouse positions are relative to it.
struct ScriptViewRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ScriptEvalResult
{
    bool ok = false;
    std::string text;   // inspected results, or the error message
};

// One editable property declared by a script's fox.script{ properties = ... }.
struct ScriptPropertyInfo
{
    std::string name;
    std::string type;            // number|integer|boolean|string|vec3|color|enum|entity|scene
    nlohmann::json defaultValue; // vec3/color as [x, y, z]; entity/no default as null
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> step;
    std::string tooltip;
    std::vector<std::string> options; // enum values
};

struct ScriptDescription
{
    bool ok = false;
    std::string error;
    std::vector<ScriptPropertyInfo> properties;
    // fox.actor classes: placeable actors with components (base classes merged).
    bool actor = false;
    nlohmann::json components = nlohmann::json::array();
};

struct ScriptTestResult
{
    std::string file;
    std::string name;
    bool passed = false;
    std::string message;   // failure text with file(line)
};

using ScriptInstanceId = uint32_t;

// Lua 5.4 scripting runtime: one sandboxed VM per session, Unity-style script
// instances with lifecycle callbacks, coroutines, hot reload and a REPL.
// Script errors are logged with file(line) and never propagate into C++: the
// failing instance stops receiving callbacks until its code is reloaded.
class ScriptSystem
{
public:
    ScriptSystem();
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Creates a fresh VM (sandbox, bindings, runtime prelude) and instantiates
    // the autoload scripts in order. Returns false only if the VM could not be
    // created; script errors are logged and counted instead.
    bool Start(const ScriptHost& host, const std::vector<std::string>& autoloads = {});
    // Calls on_destroy on every instance (reverse creation order) and closes the VM.
    void Stop();
    bool IsRunning() const;

    // Per-frame driver: hot reload, enable/disable transitions, pending
    // on_start, coroutines, on_update, the scene update (physics steps call
    // on_fixed_update, then new contacts call on_hit), then on_late_update.
    // dt is unscaled seconds; the scene advances by dt * fox.time.scale.
    // Skip it while the scene loads.
    // Once a scene is applied, Simulate only holds it (dt 0) and DrawGui
    // draws nothing until OnSceneReady has created the scene's scripts.
    void Simulate(float dt);
    // Calls on_gui inside the current ImGui frame. Begin/End imbalances and
    // errors are recovered so a broken script cannot corrupt the host UI.
    void DrawGui();

    void SetViewRect(const ScriptViewRect& view);
    // Without focus fox.input reports neutral state (nothing held, no deltas).
    void SetInputFocus(bool focused);
    bool WantsMouse() const;      // a script window is hovered or being used
    bool WantsKeyboard() const;   // a script text field has focus

    // Call on the frame SceneRuntime::TickSceneLoad reports a newly applied
    // scene: destroys the previous scene's entity scripts, instantiates each
    // actor of the new scene (its class, then its script components, in
    // entity order) and calls on_scene_loaded on autoloads (after their on_start).
    void OnSceneReady();

    // Instantiates a script component. properties holds the overrides
    // ({ "speed": 45 }); missing values come from the script's defaults.
    // ownerEntity is the runtime Entity::GetID() of the owning entity (0: none).
    // Returns 0 when the script fails to load (the error is logged).
    ScriptInstanceId CreateInstance(const std::string& path,
                                    const nlohmann::json& properties = nlohmann::json::object(),
                                    uint32_t ownerEntity = 0);
    // Calls on_destroy and removes the instance at the end of the frame.
    void DestroyInstance(ScriptInstanceId id);

    // REPL: evaluates an expression ("return <code>" first) or a statement.
    ScriptEvalResult Execute(const std::string& code);

    // Loads a script (reloading it if the file changed) and reports its
    // property schema (editor, validation). Requires a running session.
    ScriptDescription Describe(const std::string& path);

    // The components of an actor class (fox.actor, base classes merged), or
    // why it cannot be used. Start installs this as the scene's
    // SceneRuntime::actorClassResolver; Stop restores the previous resolver.
    ResolvedActorClass ResolveActorClass(const std::string& path);

    // Runs fox.test cases from the given *_test.lua files, filtered by a
    // substring of "file: case name". Requires a running session.
    std::vector<ScriptTestResult> RunTests(const std::vector<std::string>& files,
                                           const std::string& filter = {});

    // Checks hot-reload candidates now instead of on the twice-a-second poll.
    void ReloadChangedScripts();

    // Aborts any single callback that runs longer than this (default 5 s).
    void SetTimeBudget(double seconds);

    // Script errors logged since Start (load, runtime and test failures).
    uint32_t ErrorCount() const;

    // fox.time.scale; 0 pauses the scaled simulation.
    double TimeScale() const;

    // LuaLS stubs for every binding: the contents of Engine/Scripts/types/fox.lua.
    static std::string GenerateApiStubs();
    static constexpr const char* k_apiStubPath = "Engine/Scripts/types/fox.lua";

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace SE
