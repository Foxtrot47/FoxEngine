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

} // namespace

int CommandNew(ToolContext& /*context*/, const std::vector<std::string>& args)
{
    std::string name;
    std::string subdirectory;
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
    if (!WriteTextFile(scriptPath, ScriptTemplate(name), &error))
    {
        PrintDiagnostic(Severity::Error, scriptPath, error);
        return k_exitFailure;
    }
    if (!WriteTextFile(testPath, TestTemplate(name, scriptPath, module), &error))
    {
        PrintDiagnostic(Severity::Error, testPath, error);
        return k_exitFailure;
    }
    std::printf("created %s\ncreated %s\n", scriptPath.c_str(), testPath.c_str());
    return k_exitSuccess;
}

} // namespace FoxProject
