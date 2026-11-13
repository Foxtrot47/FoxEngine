#include <cctype>
#include <cstdio>
#include "Common.h"

namespace FoxProject {

namespace {

bool IsIdentifier(const std::string& text)
{
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0])))
        return false;
    for (const char c : text)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

// The --dir value: a subdirectory of Assets/Scripts whose components are
// identifiers, so the module stays requirable ("Game/Enemies" -> Game.Enemies).
bool ParseSubdirectory(const std::string& raw, std::string& out, std::string& error)
{
    std::string path = NormalizePath(raw);
    const std::string prefix = std::string(k_scriptsDirectory);
    if (path == prefix)
        path.clear();
    else if (StartsWith(path, prefix + "/"))
        path = path.substr(prefix.size() + 1);
    if (path == ".")
        path.clear();

    size_t start = 0;
    while (start < path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string component = path.substr(start, end - start);
        if (!IsIdentifier(component))
        {
            error = "--dir '" + raw + "': '" + component +
                    "' is not a valid module directory name (letters, digits and _)";
            return false;
        }
        start = end + 1;
    }
    out = path;
    return true;
}

std::string ScriptTemplate(const std::string& name)
{
    std::string text = R"(--- $NAME: describe what this script does.
--- Add it to a placed actor in the editor (Details > Add Script), or list it
--- under "autoload" in project.json to run it for the whole game.
--- API reference: the engine's Engine/Scripts/types/fox.lua (regenerate with `FoxProject api`).

local DEFAULT_SPEED = 1.0

---@class $NAME : fox.Script
---@field speed number Units per second.
local $NAME = fox.script {
    properties = {
        { name = "speed", type = "number", default = DEFAULT_SPEED, min = 0, step = 0.1,
          tooltip = "Units per second." },
    },
}

--- The declared default of `speed`, exposed for tests.
$NAME.DEFAULT_SPEED = DEFAULT_SPEED

--- Pure helper (no engine state), so tests can call it directly.
---@param speed number
---@param dt number
---@return number
function $NAME.distance(speed, dt)
    return speed * dt
end

--- Called once before the first on_update.
function $NAME:on_start()
    self.travelled = 0
end

--- Called every frame with the scaled frame time in seconds.
---@param dt number
function $NAME:on_update(dt)
    self.travelled = self.travelled + $NAME.distance(self.speed, dt)
end

return $NAME
)";
    for (size_t at = text.find("$NAME"); at != std::string::npos; at = text.find("$NAME", at + name.size()))
        text.replace(at, 5, name);
    return text;
}

std::string TestTemplate(const std::string& name, const std::string& scriptPath, const std::string& module)
{
    return "-- Tests for " + scriptPath + ". Run: FoxProject test " + name + "\n"
           "local " + name + " = require(\"" + module + "\")\n"
           "\n"
           "fox.test.case(\"speed defaults to DEFAULT_SPEED\", function()\n"
           "    fox.test.expect_eq(" + name + ".DEFAULT_SPEED, 1.0)\n"
           "end)\n"
           "\n"
           "fox.test.case(\"distance scales speed by dt\", function()\n"
           "    fox.test.expect_near(" + name + ".distance(4.0, 0.25), 1.0)\n"
           "end)\n";
}

std::string ActorTemplate(const std::string& name, const std::string& scriptPath)
{
    std::string text = R"(--- $NAME: describe what this actor is.
--- Place it in a level (editor: Outliner > Add > Actors) or spawn it from a script:
---   fox.world.spawn("$PATH", { position = vec3(0, 0, 0) })
--- Each component becomes self.<name>; a level placement can override any
--- component field and property. Add fox.script components to placements in
--- Details > Add Script. API reference: the engine's Engine/Scripts/types/fox.lua.

local DEFAULT_PULSE_SPEED = 0.5

---@class $NAME : fox.Actor
---@field light fox.ActorComponent
---@field body fox.ActorComponent
---@field pulse_speed number Light pulses per second.
local $NAME = fox.actor {
    components = {
        { name = "light", type = "point_light", color = vec3(1, 0.85, 0.6), radius = 8,
          position = vec3(0, 1.5, 0) },
        { name = "body", type = "box", size = vec3(1, 1, 1), position = vec3(0, 0.5, 0) },
    },
    properties = {
        { name = "pulse_speed", type = "number", default = DEFAULT_PULSE_SPEED, min = 0, step = 0.1,
          tooltip = "Light pulses per second." },
    },
}

--- The declared default of `pulse_speed`, exposed for tests.
$NAME.DEFAULT_PULSE_SPEED = DEFAULT_PULSE_SPEED

--- Pure helper (no engine state), so tests can call it directly.
---@param time number Seconds.
---@param speed number Pulses per second.
---@return number
function $NAME.intensity_at(time, speed)
    return 1 + 0.5 * math.sin(2 * math.pi * speed * time)
end

--- Called every frame with the scaled frame time in seconds.
---@param dt number
function $NAME:on_update(dt)
    if dt <= 0 then
        return
    end
    self.light.intensity = $NAME.intensity_at(fox.time.time, self.pulse_speed)
end

return $NAME
)";
    for (size_t at = text.find("$PATH"); at != std::string::npos; at = text.find("$PATH", at + scriptPath.size()))
        text.replace(at, 5, scriptPath);
    for (size_t at = text.find("$NAME"); at != std::string::npos; at = text.find("$NAME", at + name.size()))
        text.replace(at, 5, name);
    return text;
}

std::string ActorTestTemplate(const std::string& name, const std::string& scriptPath, const std::string& module)
{
    return "-- Tests for " + scriptPath + ". Run: FoxProject test " + name + "\n"
           "-- Headless tests have no scene: test pure helpers, not spawned actors.\n"
           "local " + name + " = require(\"" + module + "\")\n"
           "\n"
           "fox.test.case(\"pulse_speed defaults to DEFAULT_PULSE_SPEED\", function()\n"
           "    fox.test.expect_eq(" + name + ".DEFAULT_PULSE_SPEED, 0.5)\n"
           "end)\n"
           "\n"
           "fox.test.case(\"intensity peaks a quarter pulse in\", function()\n"
           "    fox.test.expect_near(" + name + ".intensity_at(0, 1), 1.0)\n"
           "    fox.test.expect_near(" + name + ".intensity_at(0.25, 1), 1.5)\n"
           "end)\n";
}

} // namespace

int CommandNew(ToolContext& /*context*/, const std::vector<std::string>& args)
{
    std::string name;
    std::string subdirectory;
    bool actor = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        if (arg == "--dir")
        {
            if (i + 1 >= args.size())
                return UsageError("new", "--dir expects a directory under Assets/Scripts");
            std::string error;
            if (!ParseSubdirectory(args[++i], subdirectory, error))
                return UsageError("new", error);
        }
        else if (arg == "--actor")
            actor = true;
        else if (!arg.empty() && arg[0] == '-')
            return UsageError("new", "unknown option '" + arg + "'");
        else if (!name.empty())
            return UsageError("new", "expected one script name");
        else
            name = arg;
    }
    if (name.empty())
        return UsageError("new", "expected a script name, e.g. FoxProject new Spinner --dir Game");
    if (!IsIdentifier(name))
        return UsageError("new", "'" + name + "' is not a valid script name (letters, digits and _, not starting with a digit)");

    const std::string directory = subdirectory.empty() ? std::string(k_scriptsDirectory)
                                                       : std::string(k_scriptsDirectory) + "/" + subdirectory;
    const std::string scriptPath = directory + "/" + name + ".lua";
    const std::string testPath = directory + "/" + name + k_testSuffix;
    std::string module = subdirectory.empty() ? name : subdirectory + "/" + name;
    for (char& c : module)
        if (c == '/') c = '.';

    bool exists = false;
    for (const std::string& path : { scriptPath, testPath })
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            PrintDiagnostic(Severity::Error, path, "already exists; refusing to overwrite");
            exists = true;
        }
    }
    if (exists)
        return k_exitFailure;

    std::string error;
    if (!WriteTextFile(scriptPath, actor ? ActorTemplate(name, scriptPath) : ScriptTemplate(name), &error))
    {
        PrintDiagnostic(Severity::Error, scriptPath, error);
        return k_exitFailure;
    }
    if (!WriteTextFile(testPath, actor ? ActorTestTemplate(name, scriptPath, module)
                                       : TestTemplate(name, scriptPath, module), &error))
    {
        PrintDiagnostic(Severity::Error, testPath, error);
        return k_exitFailure;
    }
    std::printf("created %s\ncreated %s\n", scriptPath.c_str(), testPath.c_str());
    return k_exitSuccess;
}

} // namespace FoxProject
