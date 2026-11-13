#include <cmath>
#include <cstring>
#include <tuple>
#include "Engine/Core/Logger.h"
#include "Engine/Core/Window.h"
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"

namespace SE::Scripting {

namespace {

// print and fox.log.* are raw C functions: luaL_tolstring may raise a Lua
// error (a throwing __tostring), which must not unwind through C++ frames.
// The caller's chunk name ("@Assets/Scripts/X.lua") and line become the
// Logger file/line, so script output reads like engine output.
int LogWithLevel(lua_State* L, LogLevel level)
{
    const int count = lua_gettop(L);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 1; i <= count; ++i)
    {
        if (i > 1)
            luaL_addchar(&buffer, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);

    const char* file = "lua";
    int line = 0;
    lua_Debug ar;
    for (int depth = 1; lua_getstack(L, depth, &ar); ++depth)
    {
        lua_getinfo(L, "Sl", &ar);
        if (ar.currentline > 0)
        {
            file = (ar.source[0] == '@' || ar.source[0] == '=') ? ar.source + 1 : ar.short_src;
            line = ar.currentline;
            break;
        }
    }
    Logger::Get().Log(level, file, line, "%s", lua_tostring(L, -1));
    return 0;
}

int LuaPrint(lua_State* L) { return LogWithLevel(L, LogLevel::Info); }
int LuaLogInfo(lua_State* L) { return LogWithLevel(L, LogLevel::Info); }
int LuaLogWarn(lua_State* L) { return LogWithLevel(L, LogLevel::Warning); }
int LuaLogError(lua_State* L) { return LogWithLevel(L, LogLevel::Error); }

Window& RequireWindow(ScriptContext& ctx)
{
    if (!ctx.host.window)
        Raise("fox.window is not available in this context (no game window)");
    return *ctx.host.window;
}

} // namespace

void BindCore(ScriptApi& api, ScriptContext& ctx)
{
    api.Lua().set_function("print", &LuaPrint);

    api.Module("log",
        "Engine log output (FoxEngine.log, the Debug-build console window and the editor Console).\n"
        "Lines are tagged with the calling script's file(line). The global print logs at info level.")
        .Function("info", "(...: any) -> nil", "Logs the arguments, tab-separated, at info level.", &LuaLogInfo)
        .Function("warn", "(...: any) -> nil", "Logs at warning level.", &LuaLogWarn)
        .Function("error", "(...: any) -> nil",
                  "Logs at error level without raising (use the Lua error() to abort).", &LuaLogError);

    api.Module("time", "Frame timing. Scaled values follow fox.time.scale; unscaled ones do not.")
        .ReadOnly("dt", "number", "Scaled seconds since the previous frame (0 while paused).",
                  [&ctx] { return ctx.dt; })
        .ReadOnly("unscaled_dt", "number", "Real seconds since the previous frame.",
                  [&ctx] { return ctx.unscaledDt; })
        .ReadOnly("time", "number", "Scaled seconds since the session started.",
                  [&ctx] { return ctx.time; })
        .ReadOnly("real_time", "number", "Real seconds since the session started.",
                  [&ctx] { return ctx.realTime; })
        .ReadOnly("frame", "integer", "Frames simulated since the session started.",
                  [&ctx] { return static_cast<lua_Integer>(ctx.frame); })
        .ReadOnly("fps", "number", "Frames per second from the last frame time.",
                  [&ctx] { return ctx.fps; })
        .Property("scale", "number",
                  "Time scale (read/write). 0 pauses physics, particles, scaled time and\n"
                  "fox.wait timers; on_update still runs with dt = 0 and on_gui is unaffected.",
                  [&ctx] { return ctx.scale; },
                  [&ctx](double value) {
                      if (!std::isfinite(value) || value < 0.0)
                          Raise("fox.time.scale must be a finite number >= 0 (got %g)", value);
                      ctx.scale = value;
                  });

    api.Module("app", "Application state.")
        .Function("quit", "() -> nil",
                  "Closes the game (the player exits; the editor stops Play mode).",
                  [&ctx] {
                      if (!ctx.host.requestQuit)
                          Raise("fox.app.quit is not available in this context");
                      ctx.host.requestQuit();
                  })
        .Function("is_editor", "() -> boolean", "True while playing inside the editor.",
                  [&ctx] { return ctx.host.editor; })
        .Function("is_published", "() -> boolean", "True in a published build (no console, no hot reload).",
                  [&ctx] { return ctx.host.published; })
        .Function("project_name", "() -> string", "The name from project.json.",
                  [&ctx] { return ctx.host.projectName; });

    api.Module("window", "The OS window hosting the game.")
        .Function("size", "() -> integer, integer", "Client area width and height in pixels.",
                  [&ctx] {
                      Window& window = RequireWindow(ctx);
                      return std::make_tuple(static_cast<lua_Integer>(window.GetWidth()),
                                             static_cast<lua_Integer>(window.GetHeight()));
                  })
        .Property("fullscreen", "boolean", "Borderless fullscreen (read/write; F11 toggles it too).",
                  [&ctx] { return RequireWindow(ctx).IsFullscreen(); },
                  [&ctx](bool value) {
                      Window& window = RequireWindow(ctx);
                      if (window.IsFullscreen() != value)
                          window.ToggleFullscreen();
                  })
        .Function("set_title", "(title: string) -> nil",
                  "Sets the window title (the player appends fps in development builds).",
                  [&ctx](const std::string& title) {
                      if (ctx.host.setTitle)
                          ctx.host.setTitle(title);
                  });

    // --- Implemented in Engine/Scripts/fox/runtime.lua -----------------------

    api.Module("", nullptr)
        .LuaFunction("script", "(definition?: fox.ScriptDefinition) -> fox.Script",
            "Declares a script class. Return it from the module:\n"
            "```lua\n"
            "local Spinner = fox.script { properties = {\n"
            "    { name = \"speed\", type = \"number\", default = 90 },\n"
            "} }\n"
            "function Spinner:on_update(dt) self.transform:rotate(vec3(0, self.speed * dt, 0)) end\n"
            "return Spinner\n"
            "```")
        .LuaFunction("actor", "(definition?: fox.ActorDefinition) -> fox.Actor",
            "Declares an actor class: a placeable, spawnable object built from components.\n"
            "Each component becomes self.<name> (a fox.ActorComponent). Return it from the module:\n"
            "```lua\n"
            "local Lamp = fox.actor {\n"
            "    components = {\n"
            "        { name = \"mesh\", type = \"mesh\", mesh = \"Assets/Props/Lamp.fbx\" },\n"
            "        { name = \"light\", type = \"point_light\", color = vec3(1, 0.8, 0.6), position = vec3(0, 2, 0) },\n"
            "    },\n"
            "    properties = { { name = \"flicker\", type = \"boolean\", default = false } },\n"
            "}\n"
            "function Lamp:on_update(dt) self.light.intensity = 1 + math.sin(fox.time.time * 9) * 0.2 end\n"
            "return Lamp\n"
            "```")
        .LuaFunction("inspect", "(value: any, depth?: integer) -> string",
            "Pretty-prints a value (tables sorted by key, cycles marked; default depth 3).")
        .LuaFunction("wait", "(seconds: number) -> nil",
            "Inside a coroutine: suspends for the given scaled seconds.")
        .LuaFunction("wait_frames", "(frames: integer) -> nil",
            "Inside a coroutine: suspends for the given number of frames.");

    ApiDocs::Block& script = api.DocClass("fox.Script",
        "Base of every script class. Instances are tables: store state on self\n"
        "(module-level locals reset on hot reload). Callbacks are optional methods.\n"
        "Order: on_enable, on_start, then every frame coroutines, on_update, physics\n"
        "steps (on_fixed_update), on_late_update, on_gui. A callback that raises an\n"
        "error stops the instance until its file is saved again (hot reload).",
        {}, true);
    script.fields = {
        { "entity", "fox.Entity?", "Owning entity (nil for autoloads)." },
        { "transform", "fox.Transform?", "Shortcut for self.entity.transform." },
        { "enabled", "boolean", "Disabled scripts receive no updates; toggling calls on_enable/on_disable." },
        { "on_start", "fun(self)?", "Once, before the first on_update (after the first on_enable)." },
        { "on_update", "fun(self, dt: number)?", "Every frame with scaled dt." },
        { "on_fixed_update", "fun(self, dt: number)?", "Before every physics step (dt = fox.physics.fixed_dt)." },
        { "on_late_update", "fun(self, dt: number)?", "After physics and every on_update." },
        { "on_gui", "fun(self)?", "Inside the UI frame; the only place fox.ui works." },
        { "on_enable", "fun(self)?", "When the script becomes active: on its first frame and after re-enabling." },
        { "on_disable", "fun(self)?", "When the script or its entity becomes inactive, and before on_destroy." },
        { "on_destroy", "fun(self)?",
          "When the entity is destroyed, the scene unloads or the session ends (after on_start ran)." },
        { "on_reload", "fun(self)?", "After hot reload patched the class (new properties got their defaults)." },
        { "on_scene_loaded", "fun(self, scene: fox.SceneInfo)?",
          "Autoloads only: after each scene becomes ready (after on_start)." },
    };
    script.functions = {
        { "start_coroutine", "(fn: fun(...), ...: any) -> thread",
          "Runs fn as a coroutine owned by this script: it starts immediately and is\n"
          "resumed every frame (use fox.wait/fox.wait_frames). Stops on destroy.", true },
        { "stop_coroutines", "() -> nil", "Stops every coroutine started by this script.", true },
        { "get_script", "(path: string) -> fox.Script?",
          "Another script instance on the same entity (autoloads: another autoload).", true },
    };

    ApiDocs::Block& actor = api.DocClass("fox.Actor",
        "Base of every actor class (fox.actor). An actor is a root entity (self.entity,\n"
        "self.transform) whose components are child entities. Actors get every fox.Script\n"
        "callback; on_destroy runs when the actor is destroyed or its scene unloads.",
        "fox.Script", true);
    actor.functions = {
        { "destroy", "() -> nil", "Destroys the actor at the end of this frame (fox.world.destroy).", true },
        { "is_a", "(class: fox.Actor) -> boolean", "True if the actor's class is class or derives from it.", true },
    };

    ApiDocs::Block& actorDefinition = api.DocClass("fox.ActorDefinition", "Argument of fox.actor.");
    actorDefinition.fields = {
        { "base", "fox.Actor?", "Base class: its components and properties come first; same-named entries override them." },
        { "components", "fox.ComponentSpec[]?", "The actor's parts." },
        { "properties", "fox.PropertySpec[]?", "Editable properties in editor order." },
    };

    ApiDocs::Block& component = api.DocClass("fox.ComponentSpec",
        "One actor component. Levels override any key except name and type per\n"
        "placement. Transforms are relative to the actor.");
    component.fields = {
        { "name", "string", "Becomes self.<name> (an identifier; not a property name)." },
        { "type", "\"mesh\"|\"point_light\"|\"particle_emitter\"|\"box\"|\"sphere\"", "" },
        { "position", "fox.Vec3?", "" },
        { "rotation", "fox.Vec3?", "Euler degrees." },
        { "scale", "fox.Vec3?", "" },
        { "mesh", "string?", "mesh: model path (FBX, glTF, OBJ)." },
        { "cast_shadows", "boolean?", "mesh, box, sphere (default true)." },
        { "receive_shadows", "boolean?", "mesh (default true)." },
        { "alpha_cutoff", "number?", "mesh: alpha-test threshold for textures with alpha (0 = off)." },
        { "collision", "boolean?", "mesh: kinematic triangle collider that follows the component." },
        { "color", "fox.Vec3?", "point_light: linear RGB (default white); box/sphere: albedo 0..1 (default 0.8 grey)." },
        { "intensity", "number?", "point_light (default 1)." },
        { "radius", "number?", "point_light range (default 10); sphere radius (default 0.5)." },
        { "preset", "string?", "particle_emitter: Assets/Particles/Presets/*.json." },
        { "size", "fox.Vec3?", "box: full extents (default 1, 1, 1)." },
        { "visible", "boolean?", "box/sphere: render the shape (default true); false = collider only." },
        { "body", "\"kinematic\"|\"dynamic\"?",
          "box/sphere: kinematic follows the actor (default); dynamic is simulated and moves\n"
          "the actor (one per actor; it is then the actor's only collision)." },
        { "mass", "number?", "Dynamic body mass in kg (default 1)." },
        { "friction", "number?", "Default 0.5." },
        { "restitution", "number?", "Bounciness 0..1 (default 0.2)." },
    };

    ApiDocs::Block& definition = api.DocClass("fox.ScriptDefinition", "Argument of fox.script.");
    definition.fields = {
        { "properties", "fox.PropertySpec[]?", "Editable properties in editor order." },
    };

    ApiDocs::Block& spec = api.DocClass("fox.PropertySpec",
        "One editable property. Scenes store only overrides; the default comes from\n"
        "the script, so changing it updates every instance without an override.");
    spec.fields = {
        { "name", "string", "Field name on self (not entity/transform/enabled, _* or on_*)." },
        { "type", "fox.PropertyType", "" },
        { "default", "any?", "Defaults: 0, false, \"\", vec3(0,0,0), color vec3(1,1,1), first option, nil entity." },
        { "min", "number?", "Editor slider minimum." },
        { "max", "number?", "Editor slider maximum." },
        { "step", "number?", "Editor drag speed." },
        { "tooltip", "string?", "" },
        { "options", "string[]?", "Allowed values of an enum property." },
    };
    api.DocAlias("fox.PropertyType", "Script property types and their Lua values.",
        { "\"number\"", "\"integer\"", "\"boolean\"", "\"string\"", "\"vec3\"", "\"color\" # fox.Vec3 with rgb in 0..1",
          "\"enum\" # string from options", "\"entity\" # fox.Entity or nil", "\"scene\" # scene path string" });

    api.Module("test",
        "Unit tests for *_test.lua files, run headless by `FoxProject test`.\n"
        "```lua\n"
        "fox.test.case(\"clamps pitch\", function()\n"
        "    fox.test.expect_eq(Look.clamp_pitch(120), 89)\n"
        "end)\n"
        "```")
        .LuaFunction("case", "(name: string, fn: fun()) -> nil", "Registers a test case.")
        .LuaFunction("expect", "(condition: any, message?: string) -> nil", "Fails the case when condition is falsy.")
        .LuaFunction("expect_eq", "(actual: any, expected: any) -> nil",
            "Fails unless the values are equal (tables compared deeply).")
        .LuaFunction("expect_near", "(actual: number|fox.Vec3, expected: number|fox.Vec3, epsilon?: number) -> nil",
            "Fails unless the values differ by at most epsilon (default 1e-6).");
}

} // namespace SE::Scripting
