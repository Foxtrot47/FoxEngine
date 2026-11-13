#include "Engine/Scripting/ScriptSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <imgui.h>
#include <imgui_internal.h>
#include "Engine/Core/Logger.h"
#include "Engine/Core/ProjectRoot.h"
#include "Engine/Input/InputManager.h"
#include "Engine/Input/KeyNames.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/SceneRuntime.h"
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"
#include "Scripting/ScriptTypes.h"

namespace SE {

using namespace Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* k_runtimePath       = "Engine/Scripts/fox/runtime.lua";
constexpr const char* k_scriptRoot        = "Assets/Scripts/";
constexpr double      k_reloadPollSeconds = 0.5;
constexpr int         k_hookInstructions  = 1000;

using SteadyClock = std::chrono::steady_clock;

// The per-callback time budget, read by the instruction-count hook through
// the VM's extra space (coroutine threads inherit it).
struct Budget
{
    double seconds = 5.0;
    int depth = 0;   // protected calls in flight; only the outermost sets the deadline
    SteadyClock::time_point deadline{};
};

std::string NormalizeScriptPath(const std::string& path)
{
    if (path.empty())
        return {};
    std::string result = fs::path(path).lexically_normal().generic_string();
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool ReadTextFile(const std::string& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    if (out.size() >= 3 && std::memcmp(out.data(), "\xEF\xBB\xBF", 3) == 0)
        out.erase(0, 3);
    return true;
}

bool FileWriteTime(const std::string& path, fs::file_time_type& out)
{
    std::error_code ec;
    out = fs::last_write_time(path, ec);
    return !ec;
}

// Lua error positions: "<chunk>:<line>: message" on the first line.
bool SplitLocation(std::string_view message, std::string_view* file, int* line, std::string_view* rest)
{
    const size_t lineEnd = (std::min)(message.find('\n'), message.size());
    for (size_t colon = message.find(':'); colon < lineEnd; colon = message.find(':', colon + 1))
    {
        size_t digits = colon + 1;
        int number = 0;
        while (digits < lineEnd && std::isdigit(static_cast<unsigned char>(message[digits])))
            number = number * 10 + (message[digits++] - '0');
        if (colon == 0 || digits == colon + 1 || digits >= lineEnd || message[digits] != ':')
            continue;
        if (file)
            *file = message.substr(0, colon);
        if (line)
            *line = number;
        if (rest)
        {
            size_t start = digits + 1;
            if (start < message.size() && message[start] == ' ')
                ++start;
            *rest = message.substr(start);
        }
        return true;
    }
    return false;
}

// Drops the "[C]: in ?" frame every protected call ends with, and a
// traceback header left without frames.
std::string CleanTraceback(const std::string& message)
{
    std::string out;
    out.reserve(message.size());
    bool first = true;
    size_t start = 0;
    while (start <= message.size())
    {
        size_t end = message.find('\n', start);
        if (end == std::string::npos)
            end = message.size();
        const std::string_view line(message.data() + start, end - start);
        if (line != "\t[C]: in ?")
        {
            if (!first)
                out += '\n';
            out.append(line.data(), line.size());
            first = false;
        }
        start = end + 1;
    }
    static constexpr std::string_view k_header = "\nstack traceback:";
    if (out.size() >= k_header.size() &&
        std::string_view(out).substr(out.size() - k_header.size()) == k_header)
        out.erase(out.size() - k_header.size());
    return out;
}

// The message without its traceback (inline UI errors).
std::string FirstLines(const std::string& message)
{
    const size_t traceback = message.find("\nstack traceback:");
    return traceback == std::string::npos ? message : message.substr(0, traceback);
}

std::string StackString(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return text ? std::string(text, length) : std::string("(error object is not a string)");
}

// Message handler of every protected call. Errors raised by C++ bindings
// (ScriptError) carry no position, so they get the calling script line;
// then a traceback is appended unless the message already has one
// (coroutine errors carry the coroutine's own traceback).
int ErrorHandler(lua_State* L)
{
    if (!lua_isstring(L, 1))
    {
        if (!luaL_callmeta(L, 1, "__tostring") || lua_type(L, -1) != LUA_TSTRING)
            lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        lua_replace(L, 1);
    }
    const char* message = lua_tostring(L, 1);
    if (!SplitLocation(message, nullptr, nullptr, nullptr))
    {
        lua_Debug ar;
        for (int level = 1; lua_getstack(L, level, &ar); ++level)
        {
            if (lua_getinfo(L, "Sl", &ar) && ar.currentline > 0)
            {
                message = lua_pushfstring(L, "%s:%d: %s", ar.short_src, ar.currentline, message);
                break;
            }
        }
    }
    if (std::strstr(message, "stack traceback:"))
    {
        lua_pushstring(L, message);
        return 1;
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

// Instruction-count hook: aborts a callback that overruns the budget
// (an infinite loop must not hang the editor or the game).
void BudgetHook(lua_State* L, lua_Debug*)
{
    const Budget* budget = *static_cast<Budget**>(lua_getextraspace(L));
    if (!budget || budget->depth <= 0 || SteadyClock::now() < budget->deadline)
        return;
    luaL_where(L, 0); // inside a hook, level 0 is the running function
    lua_pushfstring(L, "script ran longer than %f s (infinite loop?); the callback was aborted",
                    budget->seconds);
    lua_concat(L, 2);
    lua_error(L);
}

sol::object JsonToLua(sol::state_view lua, const json& value)
{
    switch (value.type())
    {
    case json::value_t::boolean:
        return sol::make_object(lua, value.get<bool>());
    case json::value_t::number_integer:
        return sol::make_object(lua, static_cast<lua_Integer>(value.get<int64_t>()));
    case json::value_t::number_unsigned:
        return sol::make_object(lua, static_cast<lua_Integer>(value.get<uint64_t>()));
    case json::value_t::number_float:
        return sol::make_object(lua, value.get<double>());
    case json::value_t::string:
        return sol::make_object(lua, value.get<std::string>());
    case json::value_t::array:
    {
        sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
        for (size_t i = 0; i < value.size(); ++i)
            table[i + 1] = JsonToLua(lua, value[i]);
        return sol::object(table);
    }
    case json::value_t::object:
    {
        sol::table table = lua.create_table(0, static_cast<int>(value.size()));
        for (const auto& item : value.items())
            table[item.key()] = JsonToLua(lua, item.value());
        return sol::object(table);
    }
    default:
        return sol::make_object(lua, sol::lua_nil);
    }
}

json LuaToJson(lua_State* L, const sol::object& value)
{
    switch (value.get_type())
    {
    case sol::type::boolean:
        return value.as<bool>();
    case sol::type::number:
    {
        value.push(L);
        const bool integer = lua_isinteger(L, -1) != 0;
        const lua_Integer integerValue = integer ? lua_tointeger(L, -1) : 0;
        const double number = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return integer ? json(static_cast<int64_t>(integerValue)) : json(number);
    }
    case sol::type::string:
        return value.as<std::string>();
    case sol::type::userdata:
        if (value.is<ScriptVec3>())
        {
            const ScriptVec3 v = value.as<ScriptVec3>();
            return json::array({ v.x, v.y, v.z });
        }
        return nullptr;
    default:
        return nullptr;
    }
}

std::optional<double> OptionalNumber(const sol::table& table, const char* key)
{
    const sol::object value = table.raw_get<sol::object>(key);
    if (value.get_type() != sol::type::number)
        return std::nullopt;
    return value.as<double>();
}

std::string RawString(const sol::table& table, const char* key)
{
    const sol::object value = table.raw_get<sol::object>(key);
    return value.get_type() == sol::type::string ? value.as<std::string>() : std::string();
}

} // namespace

// --- Implementation -------------------------------------------------------------

struct ScriptSystem::Impl final : ScriptInstances, PhysicsStepListener
{
    struct Instance
    {
        ScriptInstanceId id = 0;
        std::string path;
        uint32_t owner = 0;           // runtime entity id; 0 for autoloads
        uint32_t generation = 0;      // scene generation the owner belongs to
        bool autoload = false;
        sol::table self;
        bool enabledNotified = false; // on_enable ran and on_disable has not
        bool started = false;         // on_start completed
        bool failed = false;          // a callback raised: silent until reloaded
        bool destroyed = false;
        bool warnedOpenWindow = false;
        uint32_t sceneNotified = 0;   // autoloads: generation on_scene_loaded was sent for
    };

    struct Module
    {
        std::string path;
        fs::file_time_type writeTime{};
        sol::object value;            // what the chunk returned (true for nothing)
        bool ok = false;              // loaded at least once
        std::string error;            // last load error (the previous version keeps running)
        bool errorReported = false;
        bool notClassReported = false;
        bool loading = false;
    };

    // A script component that failed to load; created once its file loads.
    struct PendingCreation
    {
        std::string path;
        json properties;
        uint32_t owner = 0;
        uint32_t generation = 0;
        bool autoload = false;
    };

    // Callbacks may add or destroy instances; removal waits until the
    // outermost dispatch loop has finished.
    struct DispatchScope
    {
        explicit DispatchScope(Impl& owner) : impl(owner) { ++impl.m_dispatchDepth; }
        ~DispatchScope()
        {
            if (--impl.m_dispatchDepth == 0)
                impl.Purge();
        }
        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;
        Impl& impl;
    };

    // Bindings capture m_ctx, so it is declared before (destroyed after) the VM.
    ScriptContext m_ctx;
    Budget m_budget;
    std::unique_ptr<sol::state> m_lua;
    sol::table m_rt;
    sol::object m_rtNewInstance;
    sol::object m_rtProperties;
    sol::object m_rtPatch;
    sol::object m_rtFillDefaults;
    sol::object m_rtResumeCoroutines;
    sol::object m_rtTakeTestCases;
    sol::object m_rtInspect;
    std::map<std::string, Module> m_modules;
    std::unordered_map<std::string, std::string> m_requirePaths;
    std::vector<std::string> m_loadingStack;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::vector<PendingCreation> m_pending;
    ScriptInstanceId m_nextInstanceId = 1;
    uint32_t m_errorCount = 0;
    uint32_t m_readyGeneration = 0;
    double m_nextReloadPoll = 0.0;
    int m_dispatchDepth = 0;
    int m_quietLoads = 0;
    bool m_inSceneUpdate = false;
    bool m_running = false;
    std::string m_creationContext;  // prefixes runtime warnings while an instance is created
    std::string m_lastLoadError;

    ~Impl() { Stop(); }

    lua_State* State() const { return m_lua->lua_state(); }

    // --- Protected calls -----------------------------------------------------------

    int ProtectedCall(lua_State* L, int argumentCount, int resultCount, int handlerIndex)
    {
        if (m_budget.depth++ == 0)
            m_budget.deadline = SteadyClock::now() +
                std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(m_budget.seconds));
        const int status = lua_pcall(L, argumentCount, resultCount, handlerIndex);
        --m_budget.depth;
        return status;
    }

    // Calls fn(args...) under the time budget. On failure returns false with
    // the message (script location and traceback) in error.
    template <typename... Args>
    bool Call(const sol::object& fn, sol::object* result, std::string* error, Args&&... args)
    {
        lua_State* L = State();
        const int base = lua_gettop(L);
        lua_pushcfunction(L, &ErrorHandler);
        fn.push(L);
        ((void)sol::stack::push(L, std::forward<Args>(args)), ...);
        const int status = ProtectedCall(L, static_cast<int>(sizeof...(Args)), result ? 1 : 0, base + 1);
        if (status != LUA_OK)
        {
            if (error)
                *error = CleanTraceback(StackString(L, -1));
            lua_settop(L, base);
            return false;
        }
        if (result)
            *result = sol::object(L, -1);
        lua_settop(L, base);
        return true;
    }

    // Method lookup through raw __index tables only, so a hostile metatable
    // cannot raise outside a protected call.
    sol::object FindMethod(const sol::table& self, const char* name) const
    {
        lua_State* L = State();
        const int top = lua_gettop(L);
        self.push(L);
        for (int depth = 0; depth < 8 && lua_istable(L, -1); ++depth)
        {
            lua_pushstring(L, name);
            lua_rawget(L, -2);
            if (!lua_isnil(L, -1))
            {
                sol::object found(L, -1);
                lua_settop(L, top);
                return found;
            }
            lua_pop(L, 1);
            if (!lua_getmetatable(L, -1))
                break;
            lua_pushliteral(L, "__index");
            lua_rawget(L, -2);
            lua_replace(L, -3); // t, mt, __index -> __index, mt
            lua_pop(L, 1);
        }
        lua_settop(L, top);
        return sol::make_object(*m_lua, sol::lua_nil);
    }

    // Calls self:name(args...) when the class defines it. A failing callback
    // silences the instance until its script is reloaded.
    template <typename... Args>
    bool Callback(Instance& instance, const char* name, Args&&... args)
    {
        if (instance.failed)
            return false;
        const sol::object method = FindMethod(instance.self, name);
        if (method.get_type() != sol::type::function)
            return true;
        std::string error;
        if (Call(method, nullptr, &error, instance.self, std::forward<Args>(args)...))
            return true;
        instance.failed = true;
        ReportError(error);
        return false;
    }

    // --- Diagnostics -----------------------------------------------------------------

    // Logs with the script position as the log file/line, like engine output.
    static void LogScript(LogLevel level, const std::string& message, const std::string& fallbackFile)
    {
        std::string_view file;
        std::string_view rest;
        int line = 0;
        if (SplitLocation(message, &file, &line, &rest))
            Logger::Get().Log(level, std::string(file).c_str(), line, "%.*s",
                              static_cast<int>(rest.size()), rest.data());
        else
            Logger::Get().Log(level, fallbackFile.c_str(), 0, "%s", message.c_str());
    }

    void ReportError(const std::string& message, const std::string& fallbackFile = "lua")
    {
        ++m_errorCount;
        LogScript(LogLevel::Error, message, fallbackFile);
    }

    void ReportLoadError(Module& module)
    {
        if (m_quietLoads > 0 || module.errorReported)
            return;
        module.errorReported = true;
        // A failed require fails every module up the chain with the same text.
        if (module.error == m_lastLoadError)
            return;
        m_lastLoadError = module.error;
        ReportError(module.error, module.path);
    }

    // --- Modules ---------------------------------------------------------------------

    bool CompileFile(const std::string& path, sol::object& chunk, std::string& error)
    {
        std::string source;
        if (!ReadTextFile(path, source))
        {
            error = "cannot read '" + path + "'";
            return false;
        }
        lua_State* L = State();
        const std::string chunkName = "@" + path;
        if (luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK)
        {
            error = StackString(L, -1);
            lua_pop(L, 1);
            return false;
        }
        chunk = sol::object(L, -1);
        lua_pop(L, 1);
        return true;
    }

    bool RunModule(Module& module, sol::object& value, std::string& error)
    {
        sol::object chunk;
        if (!CompileFile(module.path, chunk, error))
            return false;
        module.loading = true;
        m_loadingStack.push_back(module.path);
        const bool ok = Call(chunk, &value, &error);
        m_loadingStack.pop_back();
        module.loading = false;
        if (ok && (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none))
            value = sol::make_object(*m_lua, true);
        return ok;
    }

    static bool IsClass(const sol::object& value)
    {
        if (value.get_type() != sol::type::table)
            return false;
        const sol::object marker = value.as<sol::table>().raw_get<sol::object>("__fox_class");
        return marker.get_type() == sol::type::boolean && marker.as<bool>();
    }

    // Script classes are named after their file for fox.inspect and errors.
    static void NameClass(Module& module)
    {
        module.notClassReported = false;
        if (!IsClass(module.value))
            return;
        sol::table cls = module.value.as<sol::table>();
        cls.raw_set("__path", module.path);
        if (RawString(cls, "__name") == "Script")
            cls.raw_set("__name", fs::path(module.path).stem().string());
    }

    // The module at path, loaded on first use. A failed module is retried
    // once its file changed; a loaded one changes only through hot reload.
    Module* EnsureModule(const std::string& path, std::string& error)
    {
        if (m_loadingStack.empty())
            m_lastLoadError.clear();
        auto [it, inserted] = m_modules.try_emplace(path);
        Module& module = it->second;
        if (inserted)
            module.path = path;
        if (module.loading)
        {
            error = "circular require: ";
            for (const std::string& loading : m_loadingStack)
                error += loading + " -> ";
            error += path;
            return nullptr;
        }
        if (module.ok)
            return &module;

        fs::file_time_type writeTime{};
        const bool exists = FileWriteTime(path, writeTime);
        if (!inserted && writeTime == module.writeTime)
        {
            error = module.error;
            ReportLoadError(module);
            return nullptr;
        }
        module.writeTime = writeTime;
        module.errorReported = false;
        if (!exists)
        {
            module.error = "script '" + path + "' does not exist (paths are relative to the project root)";
            error = module.error;
            ReportLoadError(module);
            return nullptr;
        }
        sol::object value;
        if (!RunModule(module, value, module.error))
        {
            error = module.error;
            ReportLoadError(module);
            return nullptr;
        }
        module.value = std::move(value);
        module.ok = true;
        module.error.clear();
        NameClass(module);
        return &module;
    }

    std::string ResolveModulePath(const std::string& name, std::string& error) const
    {
        std::vector<std::string> candidates;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0)
        {
            candidates.push_back(NormalizeScriptPath(name));
        }
        else
        {
            std::string relative = name;
            std::replace(relative.begin(), relative.end(), '.', '/');
            relative += ".lua";
            candidates.push_back(k_scriptRoot + relative);
            for (const std::string& root : m_ctx.host.extraSearchRoots)
                candidates.push_back(NormalizeScriptPath(root + "/" + relative));
        }
        for (const std::string& candidate : candidates)
        {
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec))
                return candidate;
        }
        error = "module '" + name + "' not found (\"Game.Player\" is Assets/Scripts/Game/Player.lua); searched:";
        for (const std::string& candidate : candidates)
            error += "\n\t" + candidate;
        return {};
    }

    // The global require: modules are cached by path and shared with script
    // components, so hot reload patches the table every requirer holds.
    sol::object RequireModule(const std::string& name)
    {
        if (name.empty())
            Raise("require expects a module name such as \"Game.Player\"");
        std::string error;
        std::string path;
        const auto cached = m_requirePaths.find(name);
        if (cached != m_requirePaths.end())
        {
            path = cached->second;
        }
        else
        {
            path = ResolveModulePath(name, error);
            if (path.empty())
                Raise("%s", error.c_str());
            m_requirePaths.emplace(name, path);
        }
        Module* module = EnsureModule(path, error);
        if (!module)
            Raise("%s", error.c_str());
        return module->value;
    }

    // Re-runs a changed module and patches the previous table in place, so
    // instances and requirers keep their references and see the new code.
    void ReloadModule(Module& module, fs::file_time_type writeTime)
    {
        module.writeTime = writeTime;
        sol::object value;
        std::string error;
        if (!RunModule(module, value, error))
        {
            module.error = error;
            module.errorReported = false;
            if (m_quietLoads == 0)
            {
                module.errorReported = true;
                ReportError(error, module.path);
            }
            return;
        }
        if (module.ok && module.value.get_type() == sol::type::table && value.get_type() == sol::type::table)
        {
            if (!Call(m_rtPatch, nullptr, &error, module.value, value))
            {
                ReportError(error, module.path);
                return;
            }
        }
        else
        {
            module.value = std::move(value);
        }
        module.ok = true;
        module.error.clear();
        NameClass(module);
        if (m_quietLoads == 0)
            Logger::Get().Log(LogLevel::Info, module.path.c_str(), 0, "reloaded");

        for (size_t i = 0; i < m_instances.size(); ++i)
        {
            Instance& instance = *m_instances[i];
            if (instance.destroyed || instance.path != module.path)
                continue;
            if (!Call(m_rtFillDefaults, nullptr, &error, instance.self))
            {
                ReportError(error, module.path);
                continue;
            }
            instance.failed = false;
            instance.warnedOpenWindow = false;
            if (instance.started)
                Callback(instance, "on_reload");
        }
        RetryPending(module.path);
    }

    void ReloadChangedScripts()
    {
        DispatchScope dispatch(*this);
        for (auto& entry : m_modules)
        {
            Module& module = entry.second;
            if (module.loading)
                continue;
            fs::file_time_type writeTime{};
            if (!FileWriteTime(module.path, writeTime) || writeTime == module.writeTime)
                continue;
            ReloadModule(module, writeTime);
        }
    }

    // --- Instances -------------------------------------------------------------------

    Entity* FindEntity(uint32_t id, uint32_t generation) const
    {
        SceneRuntime* scene = m_ctx.host.scene;
        if (!scene || !scene->HasScene() || generation != m_ctx.sceneGeneration)
            return nullptr;
        return scene->world.GetScene().FindEntity(id);
    }

    std::string CreationContext(uint32_t owner) const
    {
        if (owner == 0)
            return "autoload";
        const Entity* entity = FindEntity(owner, m_ctx.sceneGeneration);
        std::string text = "entity '" + (entity ? entity->GetName() : std::to_string(owner)) + "'";
        if (m_ctx.host.scene && !m_ctx.host.scene->currentScenePath.empty())
            text += " in " + m_ctx.host.scene->currentScenePath;
        return text;
    }

    // reported: the failure was already logged (module load errors are logged
    // once per file version, not once per instance).
    ScriptInstanceId Create(const std::string& rawPath, const sol::object& overrides, uint32_t owner,
                            bool autoload, std::string& error, bool& reported)
    {
        reported = false;
        const std::string path = NormalizeScriptPath(rawPath);
        Module* module = EnsureModule(path, error);
        if (!module)
        {
            reported = true;
            return 0;
        }
        if (!IsClass(module->value))
        {
            error = path + " does not return a script class: end the file with `return MyScript`, "
                           "where MyScript = fox.script { ... }";
            reported = module->notClassReported;
            module->notClassReported = true;
            return 0;
        }

        sol::object entity = sol::make_object(*m_lua, sol::lua_nil);
        sol::object transform = entity;
        if (owner != 0)
        {
            const ScriptEntity handle = MakeEntityHandle(m_ctx, owner);
            entity = sol::make_object(*m_lua, handle);
            transform = sol::make_object(*m_lua, ScriptTransform{ handle });
        }
        m_creationContext = CreationContext(owner);
        sol::object self;
        const bool ok = Call(m_rtNewInstance, &self, &error, module->value, overrides, path,
                             static_cast<lua_Integer>(owner), entity, transform);
        m_creationContext.clear();
        if (!ok)
            return 0;

        auto instance = std::make_unique<Instance>();
        instance->id = m_nextInstanceId++;
        instance->path = path;
        instance->owner = owner;
        instance->generation = m_ctx.sceneGeneration;
        instance->autoload = autoload;
        instance->self = self.as<sol::table>();
        const ScriptInstanceId id = instance->id;
        m_instances.push_back(std::move(instance));
        return id;
    }

    ScriptInstanceId CreateFromJson(const std::string& path, const json& properties, uint32_t owner, bool autoload)
    {
        const sol::object overrides = properties.is_object() && !properties.empty()
            ? JsonToLua(*m_lua, properties)
            : sol::make_object(*m_lua, sol::lua_nil);
        std::string error;
        bool reported = false;
        const ScriptInstanceId id = Create(path, overrides, owner, autoload, error, reported);
        if (id != 0)
            return id;
        if (!reported)
            ReportError(error, NormalizeScriptPath(path));
        m_pending.push_back({ NormalizeScriptPath(path), properties, owner, m_ctx.sceneGeneration, autoload });
        return 0;
    }

    // Creates the components that failed because this script did not load.
    void RetryPending(const std::string& path)
    {
        std::vector<PendingCreation> retry;
        for (auto it = m_pending.begin(); it != m_pending.end();)
        {
            if (it->path == path)
            {
                retry.push_back(std::move(*it));
                it = m_pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const PendingCreation& pending : retry)
        {
            if (pending.owner != 0 && !FindEntity(pending.owner, pending.generation))
                continue;
            CreateFromJson(pending.path, pending.properties, pending.owner, pending.autoload);
        }
    }

    Instance* FindInstance(ScriptInstanceId id) const
    {
        for (const auto& instance : m_instances)
            if (instance->id == id && !instance->destroyed)
                return instance.get();
        return nullptr;
    }

    void DestroyNow(Instance& instance)
    {
        if (instance.destroyed)
            return;
        instance.destroyed = true;
        if (instance.enabledNotified)
        {
            instance.enabledNotified = false;
            Callback(instance, "on_disable");
        }
        if (instance.started)
            Callback(instance, "on_destroy");
    }

    void Purge()
    {
        m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
                                         [](const std::unique_ptr<Instance>& instance) { return instance->destroyed; }),
                          m_instances.end());
    }

    static bool IsEnabled(const Instance& instance)
    {
        const sol::object enabled = instance.self.raw_get<sol::object>("enabled");
        return !(enabled.get_type() == sol::type::boolean && !enabled.as<bool>());
    }

    static bool Runnable(const Instance& instance)
    {
        return !instance.destroyed && !instance.failed && instance.started && instance.enabledNotified;
    }

    bool SceneReady() const
    {
        const SceneRuntime* scene = m_ctx.host.scene;
        return scene && scene->HasScene() && !scene->IsLoading() && m_readyGeneration == m_ctx.sceneGeneration;
    }

    // From a scene swap (or the first scene load) until the host calls
    // OnSceneReady, the scene has no entity scripts yet and autoloads have not
    // had on_scene_loaded: no callbacks run.
    bool WaitingForScene() const
    {
        const SceneRuntime* scene = m_ctx.host.scene;
        return scene && scene->HasScene() && m_readyGeneration != m_ctx.sceneGeneration;
    }

    sol::table SceneInfo()
    {
        sol::table info = m_lua->create_table(0, 2);
        info["name"] = m_ctx.host.scene->currentDesc.name;
        info["path"] = m_ctx.host.scene->currentScenePath;
        return info;
    }

    // Enable/disable transitions, pending on_start and, for autoloads, the
    // on_scene_loaded of the current scene.
    void Activate(Instance& instance)
    {
        if (instance.destroyed || instance.failed)
            return;
        bool active = IsEnabled(instance);
        if (instance.owner != 0)
        {
            const Entity* entity = FindEntity(instance.owner, instance.generation);
            if (!entity)
            {
                DestroyNow(instance);
                return;
            }
            active = active && entity->active;
        }
        if (active != instance.enabledNotified)
        {
            instance.enabledNotified = active;
            if (!Callback(instance, active ? "on_enable" : "on_disable"))
                return;
        }
        if (!active)
            return;
        if (!instance.started)
        {
            if (!Callback(instance, "on_start"))
                return;
            instance.started = true;
        }
        if (instance.autoload && instance.sceneNotified != m_ctx.sceneGeneration && SceneReady())
        {
            instance.sceneNotified = m_ctx.sceneGeneration;
            Callback(instance, "on_scene_loaded", SceneInfo());
        }
    }

    void ResumeCoroutines(Instance& instance)
    {
        if (instance.self.raw_get<sol::object>("_coroutines").get_type() != sol::type::table)
            return;
        std::string error;
        if (Call(m_rtResumeCoroutines, nullptr, &error, instance.self))
            return;
        instance.failed = true;
        ReportError(error, instance.path);
    }

    // --- Script-instance services for bindings -------------------------------------

    sol::object FindScript(sol::state_view lua, uint32_t owner, const std::string& path) override
    {
        const std::string normalized = NormalizeScriptPath(path);
        for (const auto& instance : m_instances)
            if (!instance->destroyed && instance->owner == owner && instance->path == normalized)
                return instance->self;
        return sol::make_object(lua, sol::lua_nil);
    }

    sol::object AddScript(sol::state_view lua, uint32_t owner, const std::string& path,
                          const sol::object& properties) override
    {
        const sol::type type = properties.get_type();
        if (type != sol::type::lua_nil && type != sol::type::none && type != sol::type::table)
            Raise("add_script: properties must be a table of overrides (got %s)", sol::type_name(lua, type).c_str());
        std::string error;
        bool reported = false;
        const ScriptInstanceId id = Create(path, properties, owner, false, error, reported);
        if (id == 0)
            Raise("%s", SplitLocation(error, nullptr, nullptr, nullptr) ? error.c_str()
                                                                        : ("add_script: " + error).c_str());
        return FindInstance(id)->self;
    }

    // host.resolve_entity for entity properties: scene files store the
    // entity's id in the level file.
    std::tuple<bool, sol::object> ResolveEntityProperty(const sol::object& value, sol::state_view lua)
    {
        const auto fail = [&lua](const std::string& reason) {
            return std::make_tuple(false, sol::make_object(lua, reason));
        };
        if (value.is<ScriptEntity>())
        {
            const ScriptEntity handle = value.as<ScriptEntity>();
            if (!FindEntity(handle.id, handle.generation))
                return fail("the entity no longer exists");
            return std::make_tuple(true, value);
        }
        if (value.get_type() == sol::type::number)
        {
            const double number = value.as<double>();
            if (number < 1.0 || number != std::floor(number))
                return fail("entity references are positive entity ids from the scene file");
            SceneRuntime* scene = m_ctx.host.scene;
            if (!scene || !scene->HasScene())
                return fail("no scene is loaded");
            const Entity* entity = scene->world.FindBySourceID(static_cast<LevelEntityID>(number));
            if (!entity)
                return fail("no entity with id " + std::to_string(static_cast<long long>(number)) + " in this scene");
            return std::make_tuple(true, sol::make_object(lua, MakeEntityHandle(m_ctx, entity->GetID())));
        }
        return fail("expected an entity id or fox.Entity, got " + sol::type_name(lua, value.get_type()));
    }

    // --- Physics steps -------------------------------------------------------------

    void OnPhysicsStep(PhysicsWorld&, float dt) override
    {
        // Only steps driven by Simulate; the editor steps the scene itself in edit mode.
        if (!m_running || !m_inSceneUpdate)
            return;
        DispatchScope dispatch(*this);
        const double frameDt = m_ctx.dt;
        m_ctx.dt = dt;
        for (size_t i = 0; i < m_instances.size(); ++i)
        {
            Instance& instance = *m_instances[i];
            if (Runnable(instance))
                Callback(instance, "on_fixed_update", static_cast<double>(dt));
        }
        m_ctx.dt = frameDt;
    }

    // --- Session ---------------------------------------------------------------------

    void BindActions()
    {
        for (const auto& [action, keys] : m_ctx.host.inputActions)
        {
            m_ctx.actionNames.insert(action);
            for (const std::string& name : keys)
            {
                KeyCode code;
                if (!ParseKeyName(name, code))
                {
                    SE_LOG_WARN("Input action '%s': unknown key '%s'", action.c_str(), name.c_str());
                    continue;
                }
                if (code.device == KeyCode::Device::Gamepad)
                    for (uint32_t pad = 0; pad < k_MaxGamepads; ++pad)
                        m_ctx.actions.BindGamepad(action, code.code, pad);
                else
                    m_ctx.actions.Bind(action, code.code);
            }
        }
    }

    // No file, process or bytecode access: scripts reach the engine only
    // through fox.*.
    void Sandbox()
    {
        sol::table globals = m_lua->globals();
        for (const char* name : { "dofile", "loadfile", "load" })
            globals[name] = sol::lua_nil;

        sol::table stringLib = globals["string"];
        stringLib["dump"] = sol::lua_nil;

        sol::table osLib = globals["os"];
        sol::table safeOs = m_lua->create_table();
        for (const char* name : { "clock", "time", "date", "difftime" })
            safeOs[name] = osLib.get<sol::object>(name);
        globals["os"] = safeOs;

        sol::table debugLib = globals["debug"];
        sol::table safeDebug = m_lua->create_table();
        safeDebug["traceback"] = debugLib.get<sol::object>("traceback");
        globals["debug"] = safeDebug;

        // sol2 opens the base library as global "base" (an alias of _G).
        // Registered under Lua's own name instead, tracebacks name functions
        // plainly ("in function 'error'", not 'base.error').
        globals["base"] = sol::lua_nil;
        lua_State* L = State();
        luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        lua_pushnil(L);
        lua_setfield(L, -2, "base");
        lua_pushglobaltable(L);
        lua_setfield(L, -2, LUA_GNAME);
        lua_pop(L, 1);
    }

    bool LoadRuntime(std::string& error)
    {
        sol::object chunk;
        if (!CompileFile(EnginePath(k_runtimePath).string(), chunk, error))
            return false;

        sol::table hostApi = m_lua->create_table();
        hostApi.set_function("warn", [this](const std::string& path, const std::string& message) {
            const std::string text = m_creationContext.empty() ? message : m_creationContext + ": " + message;
            Logger::Get().Log(LogLevel::Warning, path.c_str(), 0, "%s", text.c_str());
        });
        hostApi.set_function("resolve_entity", [this](const sol::object& value, sol::this_state s) {
            return ResolveEntityProperty(value, sol::state_view(s.lua_state()));
        });
        hostApi.set_function("find_script", [this](lua_Integer owner, const std::string& path, sol::this_state s) {
            return FindScript(sol::state_view(s.lua_state()), static_cast<uint32_t>(owner), path);
        });

        sol::object result;
        if (!Call(chunk, &result, &error, hostApi))
            return false;
        if (result.get_type() != sol::type::table)
        {
            error = std::string(k_runtimePath) + " must return the runtime table";
            return false;
        }
        m_rt = result.as<sol::table>();
        const std::pair<const char*, sol::object*> functions[] = {
            { "new_instance", &m_rtNewInstance },
            { "properties", &m_rtProperties },
            { "patch", &m_rtPatch },
            { "fill_defaults", &m_rtFillDefaults },
            { "resume_coroutines", &m_rtResumeCoroutines },
            { "take_test_cases", &m_rtTakeTestCases },
            { "inspect", &m_rtInspect },
        };
        for (const auto& [name, slot] : functions)
        {
            *slot = m_rt.raw_get<sol::object>(name);
            if (slot->get_type() != sol::type::function)
            {
                error = std::string(k_runtimePath) + " does not define rt." + name;
                return false;
            }
        }
        return true;
    }

    // Builds the VM. ScriptApi holds references into it, so it lives only here.
    bool CreateVm(std::string& error)
    {
        m_lua = std::make_unique<sol::state>();
        lua_State* L = State();
        *static_cast<Budget**>(lua_getextraspace(L)) = &m_budget;
        m_lua->open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string,
                              sol::lib::table, sol::lib::utf8, sol::lib::os, sol::lib::debug);
        Sandbox();
        lua_sethook(L, &BudgetHook, LUA_MASKCOUNT, k_hookInstructions);

        ApiDocs docs;
        ScriptApi api(*m_lua, docs);
        RegisterAllBindings(api, m_ctx);
        m_lua->set_function("require", [this](const std::string& name) { return RequireModule(name); });
        if (!LoadRuntime(error))
            return false;
        api.Finalize();
        return true;
    }

    bool Start(const ScriptHost& host, const std::vector<std::string>& autoloads)
    {
        Stop();
        m_ctx = ScriptContext{};
        m_ctx.host = host;
        m_ctx.instances = this;
        BindActions();
        m_errorCount = 0;
        m_readyGeneration = 0;
        m_nextReloadPoll = 0.0;

        std::string error;
        if (!CreateVm(error))
        {
            SE_LOG_ERROR("Scripting: could not start the Lua runtime: %s", error.c_str());
            Stop();
            return false;
        }
        m_running = true;
        if (m_ctx.host.scene)
            m_ctx.host.scene->onSceneUnloading = [this] { OnSceneUnloading(); };

        DispatchScope dispatch(*this);
        for (const std::string& path : autoloads)
            CreateFromJson(path, json::object(), 0, true);
        return true;
    }

    void Stop()
    {
        if (!m_lua)
            return;
        if (m_running)
        {
            DispatchScope dispatch(*this);
            for (size_t i = m_instances.size(); i-- > 0;)
                DestroyNow(*m_instances[i]);
        }
        m_running = false;
        if (SceneRuntime* scene = m_ctx.host.scene)
        {
            scene->onSceneUnloading = nullptr;
            scene->physicsWorld.RemoveStepListener(this);
        }
        if (m_ctx.host.input && m_ctx.host.input->IsCursorCaptured())
            m_ctx.host.input->SetCursorCaptured(false);

        // Every reference must be released before the VM closes.
        m_instances.clear();
        m_pending.clear();
        m_modules.clear();
        m_requirePaths.clear();
        m_loadingStack.clear();
        m_rt = sol::table();
        for (sol::object* slot : { &m_rtNewInstance, &m_rtProperties, &m_rtPatch, &m_rtFillDefaults,
                                   &m_rtResumeCoroutines, &m_rtTakeTestCases, &m_rtInspect })
            *slot = sol::object();
        m_lua.reset();
        m_budget.depth = 0;
        m_ctx = ScriptContext{};
    }

    // --- Scene transitions -----------------------------------------------------------

    void OnSceneUnloading()
    {
        if (!m_running)
            return;
        {
            DispatchScope dispatch(*this);
            for (size_t i = m_instances.size(); i-- > 0;)
                if (m_instances[i]->owner != 0)
                    DestroyNow(*m_instances[i]);
        }
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [](const PendingCreation& pending) { return pending.owner != 0; }),
                        m_pending.end());
        ++m_ctx.sceneGeneration;
        m_readyGeneration = 0;
    }

    void OnSceneReady()
    {
        SceneRuntime* scene = m_ctx.host.scene;
        if (!m_running || !scene || !scene->HasScene())
            return;
        DispatchScope dispatch(*this);
        for (size_t i = m_instances.size(); i-- > 0;)
            if (m_instances[i]->owner != 0 && m_instances[i]->generation != m_ctx.sceneGeneration)
                DestroyNow(*m_instances[i]);
        m_readyGeneration = m_ctx.sceneGeneration;
        scene->physicsWorld.AddStepListener(this);

        // Copied first: module chunks run below and may create entities.
        std::vector<std::pair<uint32_t, std::vector<LevelScriptDefinition>>> components;
        for (const auto& entity : scene->world.GetScene().GetEntities())
            if (const auto* component = entity->GetComponent<ScriptComponent>())
                components.emplace_back(entity->GetID(), component->scripts);
        for (const auto& [owner, scripts] : components)
            for (const LevelScriptDefinition& definition : scripts)
                CreateFromJson(definition.script, definition.properties, owner, false);

        for (size_t i = 0; i < m_instances.size(); ++i)
            if (m_instances[i]->autoload)
                Activate(*m_instances[i]);
    }

    // --- Frame -----------------------------------------------------------------------

    void Simulate(float frameDt)
    {
        if (!m_running)
            return;
        SceneRuntime* scene = m_ctx.host.scene;
        if (WaitingForScene())
        {
            scene->Update(0.0f, static_cast<float>(m_ctx.time)); // the scene holds
            return;
        }
        DispatchScope dispatch(*this);
        const double dt = frameDt > 0.0f ? static_cast<double>(frameDt) : 0.0;
        m_ctx.unscaledDt = dt;
        m_ctx.dt = dt * m_ctx.scale;
        m_ctx.realTime += dt;
        m_ctx.time += m_ctx.dt;
        ++m_ctx.frame;
        m_ctx.fps = dt > 0.0 ? 1.0 / dt : 0.0;
        if (m_ctx.host.input)
            m_ctx.actions.Update(*m_ctx.host.input);
        if (m_ctx.host.hotReload && m_ctx.realTime >= m_nextReloadPoll)
        {
            m_nextReloadPoll = m_ctx.realTime + k_reloadPollSeconds;
            ReloadChangedScripts();
        }

        for (size_t i = 0; i < m_instances.size(); ++i)
            Activate(*m_instances[i]);
        for (size_t i = 0; i < m_instances.size(); ++i)
            if (Runnable(*m_instances[i]))
                ResumeCoroutines(*m_instances[i]);
        for (size_t i = 0; i < m_instances.size(); ++i)
            if (Runnable(*m_instances[i]))
                Callback(*m_instances[i], "on_update", m_ctx.dt);

        if (scene && scene->HasScene() && !scene->IsLoading())
        {
            scene->physicsWorld.AddStepListener(this); // every scene load replaces the world
            m_inSceneUpdate = true;
            scene->Update(static_cast<float>(m_ctx.dt), static_cast<float>(m_ctx.time));
            m_inSceneUpdate = false;
        }

        for (size_t i = 0; i < m_instances.size(); ++i)
            if (Runnable(*m_instances[i]))
                Callback(*m_instances[i], "on_late_update", m_ctx.dt);
    }

    // A click-through window covering the game view, submitted before script
    // windows so its draw list (fox.ui.draw_*, dim) sits behind them.
    ImDrawList* BeginOverlay()
    {
        const ImVec2 origin(m_ctx.view.x, m_ctx.view.y);
        const ImVec2 size((std::max)(m_ctx.view.width, 1.0f), (std::max)(m_ctx.view.height, 1.0f));
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##FoxScriptOverlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImDrawList* list = ImGui::GetWindowDrawList();
        ImGui::End();
        ImGui::PopStyleVar(2);
        list->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y));
        return list;
    }

    static void RecoverImGui(const ImGuiErrorRecoveryState& state)
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool assertOn = io.ConfigErrorRecoveryEnableAssert;
        const bool logOn = io.ConfigErrorRecoveryEnableDebugLog;
        const bool tooltipOn = io.ConfigErrorRecoveryEnableTooltip;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        ImGui::ErrorRecoveryTryToRecoverState(&state);
        io.ConfigErrorRecoveryEnableAssert = assertOn;
        io.ConfigErrorRecoveryEnableDebugLog = logOn;
        io.ConfigErrorRecoveryEnableTooltip = tooltipOn;
    }

    void DrawGui()
    {
        if (!m_running)
            return;
        if (WaitingForScene())
        {
            m_ctx.uiWantsMouse = false;
            m_ctx.uiWantsKeyboard = false;
            return;
        }
        DispatchScope dispatch(*this);
        m_ctx.uiWantsMouseNext = false;
        m_ctx.uiWantsKeyboardNext = false;
        m_ctx.overlay = BeginOverlay();
        m_ctx.inGui = true;
        for (size_t i = 0; i < m_instances.size(); ++i)
        {
            Instance& instance = *m_instances[i];
            if (!Runnable(instance) || FindMethod(instance.self, "on_gui").get_type() != sol::type::function)
                continue;
            ImGuiErrorRecoveryState state;
            ImGui::ErrorRecoveryStoreState(&state);
            m_ctx.uiWindowDepth = 0;
            const bool ok = Callback(instance, "on_gui");
            if (ok && m_ctx.uiWindowDepth > 0 && !instance.warnedOpenWindow)
            {
                instance.warnedOpenWindow = true;
                Logger::Get().Log(LogLevel::Warning, instance.path.c_str(), 0,
                                  "on_gui returned with %d window(s) open: call fox.ui.end_window after every "
                                  "fox.ui.begin_window (closed automatically)", m_ctx.uiWindowDepth);
            }
            if (!ok || m_ctx.uiWindowDepth > 0)
                RecoverImGui(state);
            m_ctx.uiWindowDepth = 0;
        }
        m_ctx.inGui = false;
        m_ctx.overlay->PopClipRect();
        m_ctx.overlay = nullptr;
        m_ctx.uiWantsMouse = m_ctx.uiWantsMouseNext;
        m_ctx.uiWantsKeyboard = m_ctx.uiWantsKeyboardNext;
    }

    // --- Tools -----------------------------------------------------------------------

    ScriptEvalResult Execute(const std::string& code)
    {
        ScriptEvalResult result;
        if (!m_running)
        {
            result.text = "the script session is not running";
            return result;
        }
        DispatchScope dispatch(*this);
        lua_State* L = State();
        const int base = lua_gettop(L);
        lua_pushcfunction(L, &ErrorHandler);
        // Expressions first, so "fox.time.frame" prints its value.
        const std::string expression = "return " + code;
        if (luaL_loadbufferx(L, expression.data(), expression.size(), "=console", "t") != LUA_OK)
        {
            lua_pop(L, 1);
            if (luaL_loadbufferx(L, code.data(), code.size(), "=console", "t") != LUA_OK)
            {
                result.text = StackString(L, -1);
                lua_settop(L, base);
                return result;
            }
        }
        if (ProtectedCall(L, 0, LUA_MULTRET, base + 1) != LUA_OK)
        {
            result.text = CleanTraceback(StackString(L, -1));
            lua_settop(L, base);
            return result;
        }
        std::vector<sol::object> values;
        for (int index = base + 2; index <= lua_gettop(L); ++index)
            values.emplace_back(L, index);
        lua_settop(L, base);

        result.ok = true;
        for (const sol::object& value : values)
        {
            sol::object text;
            std::string error;
            if (!result.text.empty())
                result.text += ", ";
            if (Call(m_rtInspect, &text, &error, value) && text.get_type() == sol::type::string)
                result.text += text.as<std::string>();
            else
                result.text += "<cannot inspect: " + FirstLines(error) + ">";
        }
        return result;
    }

    ScriptDescription Describe(const std::string& file)
    {
        ScriptDescription description;
        if (!m_running)
        {
            description.error = "the script session is not running";
            return description;
        }
        DispatchScope dispatch(*this);
        struct QuietScope
        {
            explicit QuietScope(int& counter) : depth(counter) { ++depth; }
            ~QuietScope() { --depth; }
            QuietScope(const QuietScope&) = delete;
            QuietScope& operator=(const QuietScope&) = delete;
            int& depth;
        } quiet(m_quietLoads);

        const std::string path = NormalizeScriptPath(file);
        // Describing sessions have no hot-reload poll: pick up edits here.
        const auto existing = m_modules.find(path);
        if (existing != m_modules.end() && existing->second.ok && !existing->second.loading)
        {
            fs::file_time_type writeTime{};
            if (FileWriteTime(path, writeTime) && writeTime != existing->second.writeTime)
                ReloadModule(existing->second, writeTime);
        }
        std::string error;
        const Module* module = EnsureModule(path, error);
        if (!module)
        {
            description.error = FirstLines(error);
            return description;
        }
        if (!module->error.empty()) // the last save does not load; the previous version still runs
        {
            description.error = FirstLines(module->error);
            return description;
        }
        if (!IsClass(module->value))
        {
            description.error = path + " does not return a script class (end it with `return MyScript`)";
            return description;
        }
        sol::object list;
        if (!Call(m_rtProperties, &list, &error, module->value) || list.get_type() != sol::type::table)
        {
            description.error = FirstLines(error);
            return description;
        }

        lua_State* L = State();
        const sol::table specs = list.as<sol::table>();
        for (int index = 1;; ++index)
        {
            const sol::object entry = specs.raw_get<sol::object>(index);
            if (entry.get_type() != sol::type::table)
                break;
            const sol::table spec = entry.as<sol::table>();
            ScriptPropertyInfo info;
            info.name = RawString(spec, "name");
            info.type = RawString(spec, "type");
            info.defaultValue = LuaToJson(L, spec.raw_get<sol::object>("default"));
            info.min = OptionalNumber(spec, "min");
            info.max = OptionalNumber(spec, "max");
            info.step = OptionalNumber(spec, "step");
            info.tooltip = RawString(spec, "tooltip");
            const sol::object options = spec.raw_get<sol::object>("options");
            if (options.get_type() == sol::type::table)
            {
                const sol::table optionList = options.as<sol::table>();
                for (int option = 1;; ++option)
                {
                    const sol::object value = optionList.raw_get<sol::object>(option);
                    if (value.get_type() != sol::type::string)
                        break;
                    info.options.push_back(value.as<std::string>());
                }
            }
            description.properties.push_back(std::move(info));
        }
        description.ok = true;
        return description;
    }

    std::vector<ScriptTestResult> RunTests(const std::vector<std::string>& files, const std::string& filter)
    {
        std::vector<ScriptTestResult> results;
        if (!m_running)
            return results;
        DispatchScope dispatch(*this);
        ReloadChangedScripts();
        for (const std::string& file : files)
        {
            const std::string path = NormalizeScriptPath(file);
            std::string error;
            Call(m_rtTakeTestCases, nullptr, nullptr); // drop cases of an earlier failed file
            sol::object chunk;
            if (!CompileFile(path, chunk, error) || !Call(chunk, nullptr, &error))
            {
                ReportError(error, path);
                continue;
            }
            sol::object cases;
            if (!Call(m_rtTakeTestCases, &cases, &error) || cases.get_type() != sol::type::table)
            {
                ReportError(error, path);
                continue;
            }
            const sol::table list = cases.as<sol::table>();
            for (int index = 1;; ++index)
            {
                const sol::object entry = list.raw_get<sol::object>(index);
                if (entry.get_type() != sol::type::table)
                    break;
                const sol::table testCase = entry.as<sol::table>();
                ScriptTestResult result;
                result.file = path;
                result.name = RawString(testCase, "name");
                if (!filter.empty() && (path + ": " + result.name).find(filter) == std::string::npos)
                    continue;
                std::string failure;
                result.passed = Call(testCase.raw_get<sol::object>("fn"), nullptr, &failure);
                if (!result.passed)
                {
                    result.message = failure;
                    ++m_errorCount;
                }
                results.push_back(std::move(result));
            }
        }
        return results;
    }
};

// --- ScriptSystem -------------------------------------------------------------------

ScriptSystem::ScriptSystem() : m_impl(std::make_unique<Impl>()) {}

ScriptSystem::~ScriptSystem() = default;

bool ScriptSystem::Start(const ScriptHost& host, const std::vector<std::string>& autoloads)
{
    return m_impl->Start(host, autoloads);
}

void ScriptSystem::Stop()
{
    m_impl->Stop();
}

bool ScriptSystem::IsRunning() const
{
    return m_impl->m_running;
}

void ScriptSystem::Simulate(float dt)
{
    m_impl->Simulate(dt);
}

void ScriptSystem::DrawGui()
{
    m_impl->DrawGui();
}

void ScriptSystem::SetViewRect(const ScriptViewRect& view)
{
    m_impl->m_ctx.view = view;
}

void ScriptSystem::SetInputFocus(bool focused)
{
    Impl& impl = *m_impl;
    impl.m_ctx.inputFocus = focused;
    if (!focused && impl.m_running && impl.m_ctx.host.input && impl.m_ctx.host.input->IsCursorCaptured())
        impl.m_ctx.host.input->SetCursorCaptured(false);
}

bool ScriptSystem::WantsMouse() const
{
    return m_impl->m_running && m_impl->m_ctx.uiWantsMouse;
}

bool ScriptSystem::WantsKeyboard() const
{
    return m_impl->m_running && m_impl->m_ctx.uiWantsKeyboard;
}

void ScriptSystem::OnSceneReady()
{
    m_impl->OnSceneReady();
}

ScriptInstanceId ScriptSystem::CreateInstance(const std::string& path, const nlohmann::json& properties,
                                              uint32_t ownerEntity)
{
    if (!m_impl->m_running)
    {
        SE_LOG_ERROR("Scripting: CreateInstance(%s) without a running session", path.c_str());
        return 0;
    }
    Impl::DispatchScope dispatch(*m_impl);
    const uint32_t generation = m_impl->m_ctx.sceneGeneration;
    if (ownerEntity != 0 && !m_impl->FindEntity(ownerEntity, generation))
    {
        SE_LOG_ERROR("Scripting: CreateInstance(%s): entity %u does not exist", path.c_str(), ownerEntity);
        return 0;
    }
    return m_impl->CreateFromJson(path, properties, ownerEntity, false);
}

void ScriptSystem::DestroyInstance(ScriptInstanceId id)
{
    Impl& impl = *m_impl;
    if (!impl.m_running)
        return;
    Impl::DispatchScope dispatch(impl);
    for (size_t i = 0; i < impl.m_instances.size(); ++i)
    {
        if (impl.m_instances[i]->id == id)
        {
            impl.DestroyNow(*impl.m_instances[i]);
            break;
        }
    }
}

ScriptEvalResult ScriptSystem::Execute(const std::string& code)
{
    return m_impl->Execute(code);
}

ScriptDescription ScriptSystem::Describe(const std::string& path)
{
    return m_impl->Describe(path);
}

std::vector<ScriptTestResult> ScriptSystem::RunTests(const std::vector<std::string>& files,
                                                     const std::string& filter)
{
    return m_impl->RunTests(files, filter);
}

void ScriptSystem::ReloadChangedScripts()
{
    if (m_impl->m_running)
        m_impl->ReloadChangedScripts();
}

void ScriptSystem::SetTimeBudget(double seconds)
{
    if (seconds > 0.0 && std::isfinite(seconds))
        m_impl->m_budget.seconds = seconds;
}

uint32_t ScriptSystem::ErrorCount() const
{
    return m_impl->m_errorCount;
}

double ScriptSystem::TimeScale() const
{
    return m_impl->m_ctx.scale;
}

std::string ScriptSystem::GenerateApiStubs()
{
    // Bindings capture the context, so it outlives the state.
    ScriptContext ctx;
    ctx.headless = true;
    sol::state lua;
    ApiDocs docs;
    ScriptApi api(lua, docs);
    RegisterAllBindings(api, ctx);
    return docs.Emit();
}

} // namespace SE
