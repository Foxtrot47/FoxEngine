#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include "Common.h"
#include "Engine/Core/Project.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace FoxProject {

namespace {

using json = nlohmann::json;

struct SceneCheck
{
    std::string path;
    bool exists = false;
    SE::LevelAssetLoadResult result;
};

std::string JsonTypeName(const json& value)
{
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    return value.type_name();
}

std::string Expected(const std::string& what, const json& value)
{
    return "expected " + what + ", got " + JsonTypeName(value);
}

bool IsIntegral(const json& value)
{
    if (value.is_number_integer())
        return true;
    if (!value.is_number_float())
        return false;
    const double number = value.get<double>();
    return std::isfinite(number) && std::floor(number) == number;
}

std::string Join(const std::vector<std::string>& items, const char* separator)
{
    std::string text;
    for (const std::string& item : items)
    {
        if (!text.empty())
            text += separator;
        text += item;
    }
    return text;
}

// Mirrors the runtime coercion in Engine/Scripts/fox/runtime.lua so a scene
// that passes check never triggers "using the default" warnings at play time.
// Returns an empty string when the value is acceptable.
std::string ValidatePropertyValue(const SE::ScriptPropertyInfo& property, const json& value,
                                  const std::set<uint64_t>& entityIds)
{
    const std::string& type = property.type;
    if (type == "number")
        return value.is_number() ? std::string() : Expected("number", value);
    if (type == "integer")
        return IsIntegral(value) ? std::string() : Expected("integer", value);
    if (type == "boolean")
        return value.is_boolean() ? std::string() : Expected("boolean", value);
    if (type == "string")
        return value.is_string() ? std::string() : Expected("string", value);
    if (type == "vec3" || type == "color")
    {
        const bool ok = value.is_array() && value.size() == 3 &&
                        std::all_of(value.begin(), value.end(), [](const json& item) { return item.is_number(); });
        return ok ? std::string() : Expected("an array of 3 numbers", value);
    }
    if (type == "enum")
    {
        if (value.is_string() &&
            std::find(property.options.begin(), property.options.end(), value.get<std::string>()) !=
                property.options.end())
            return {};
        return "expected one of: " + Join(property.options, ", ");
    }
    if (type == "entity")
    {
        if (value.is_null())
            return {};
        if (!value.is_number_integer())
            return Expected("an entity id (integer)", value);
        if (value.is_number_unsigned() || value.get<int64_t>() >= 0)
        {
            const uint64_t id = value.get<uint64_t>();
            if (entityIds.count(id) != 0)
                return {};
        }
        return "entity " + value.dump() + " does not exist in this level";
    }
    if (type == "scene")
    {
        if (!value.is_string())
            return Expected("a scene path (string)", value);
        const std::string path = NormalizePath(value.get<std::string>());
        if (path.empty())
            return {};
        if (!EndsWith(ToLower(path), ".json"))
            return "'" + path + "' is not a .json scene";
        if (!IsRegularFile(path))
            return "scene '" + path + "' does not exist";
        return {};
    }
    // A type this tool does not know yet: the runtime validates it at play time.
    return {};
}

class ScriptChecker
{
public:
    ScriptChecker(SE::ScriptSystem& scripts, ConsoleLogSink& logSink, CheckReport& report)
        : m_scripts(scripts), m_log(logSink), m_report(report) {}

    // Loads a script component once and reports its load errors once.
    const SE::ScriptDescription& Describe(const std::string& path)
    {
        const auto cached = m_descriptions.find(path);
        if (cached != m_descriptions.end())
            return cached->second;

        m_log.BeginCapture();
        SE::ScriptDescription description = m_scripts.Describe(path);
        const std::vector<CapturedLog> captured = m_log.EndCapture();
        std::vector<std::string> reported;
        if (!description.ok)
        {
            const std::string error = description.error.empty() ? "script failed to load" : description.error;
            PrintDiagnostic(Severity::Error, path, error);
            reported.push_back(error);
            ++m_report.errors;
        }
        m_report.errors += PrintCapturedErrors(captured, reported, stdout);
        return m_descriptions.emplace(path, std::move(description)).first->second;
    }

    // Modules that are not script components only have to load.
    void Require(const std::string& path, const std::string& module)
    {
        m_log.BeginCapture();
        const SE::ScriptEvalResult result = m_scripts.Execute("return require('" + module + "')");
        const std::vector<CapturedLog> captured = m_log.EndCapture();
        std::vector<std::string> reported;
        if (!result.ok)
        {
            const std::string error = result.text.empty() ? "module failed to load" : result.text;
            PrintDiagnostic(Severity::Error, path, error);
            reported.push_back(error);
            ++m_report.errors;
        }
        m_report.errors += PrintCapturedErrors(captured, reported, stdout);
    }

    const std::map<std::string, SE::ScriptDescription>& Descriptions() const { return m_descriptions; }

private:
    SE::ScriptSystem& m_scripts;
    ConsoleLogSink& m_log;
    CheckReport& m_report;
    std::map<std::string, SE::ScriptDescription> m_descriptions;
};

std::vector<std::string> ScenesToCheck(const SE::ProjectSettings& project)
{
    std::set<std::string> scenes;
    for (const std::string& scene : SE::Project::SceneList(project))
        scenes.insert(NormalizePath(scene));
    for (const std::string& scene : SE::SceneLoader::ScanSceneDirectory(k_scenesDirectory))
        scenes.insert(NormalizePath(scene));
    return { scenes.begin(), scenes.end() };
}

void CheckEntityScripts(const SceneCheck& scene, ScriptChecker& checker, CheckReport& report,
                        std::set<std::string>& referencedScenes)
{
    const auto error = [&](const std::string& where, const std::string& message) {
        PrintDiagnostic(Severity::Error, scene.path, where + ": " + message);
        ++report.errors;
    };

    std::set<uint64_t> entityIds;
    for (const SE::LevelEntityDefinition& entity : scene.result.asset.entities)
        entityIds.insert(entity.id);

    const auto& entities = scene.result.asset.entities;
    for (size_t e = 0; e < entities.size(); ++e)
    {
        for (size_t s = 0; s < entities[e].scripts.size(); ++s)
        {
            const SE::LevelScriptDefinition& script = entities[e].scripts[s];
            const std::string base = "/entities/" + std::to_string(e) + "/scripts/" + std::to_string(s);
            if (script.script.empty())
                continue;
            if (!IsRegularFile(script.script))
            {
                error(base + "/script", "script '" + script.script + "' does not exist");
                continue;
            }
            const SE::ScriptDescription& description = checker.Describe(script.script);
            if (!description.ok)
            {
                error(base + "/script", "script '" + script.script + "' failed to load (see above)");
                continue;
            }
            if (!script.properties.is_object())
            {
                error(base + "/properties", "expected an object");
                continue;
            }
            for (const auto& item : script.properties.items())
            {
                const std::string& key = item.key();
                const json& value = item.value();
                const std::string where = base + "/properties/" + key;
                const auto property = std::find_if(description.properties.begin(), description.properties.end(),
                    [&](const SE::ScriptPropertyInfo& info) { return info.name == key; });
                if (property == description.properties.end())
                {
                    std::vector<std::string> names;
                    for (const SE::ScriptPropertyInfo& info : description.properties)
                        names.push_back(info.name);
                    error(where, "'" + script.script + "' declares no property '" + key + "'" +
                                 (names.empty() ? std::string(" (it declares none)")
                                                : " (declared: " + Join(names, ", ") + ")"));
                    continue;
                }
                const std::string problem = ValidatePropertyValue(*property, value, entityIds);
                if (!problem.empty())
                    error(where, problem);
                else if (property->type == "scene" && value.is_string() && !value.get<std::string>().empty())
                    referencedScenes.insert(NormalizePath(value.get<std::string>()));
            }
        }
    }
}

} // namespace

CheckReport RunCheck(ToolContext& context)
{
    CheckReport report;

    // (a) project.json
    const SE::ProjectLoadResult project = SE::Project::Load();
    const std::string projectPrefix = std::string(SE::Project::k_fileName) + ": ";
    for (const std::string& error : project.errors)
    {
        PrintDiagnostic(Severity::Error, SE::Project::k_fileName,
                        StartsWith(error, projectPrefix) ? error.substr(projectPrefix.size()) : error);
        ++report.errors;
    }

    // Scenes are parsed first: their entity scripts decide whether a script is
    // validated as a component (Describe) or as a plain module (require).
    std::vector<SceneCheck> scenes;
    std::set<std::string> components(project.project.autoloads.begin(), project.project.autoloads.end());
    for (const std::string& path : ScenesToCheck(project.project))
    {
        SceneCheck scene;
        scene.path = path;
        scene.exists = IsRegularFile(path);
        if (scene.exists)
        {
            scene.result = SE::LevelAssetLoader::LoadFromFile(path);
            for (const SE::LevelEntityDefinition& entity : scene.result.asset.entities)
                for (const SE::LevelScriptDefinition& script : entity.scripts)
                    if (!script.script.empty())
                        components.insert(script.script);
        }
        scenes.push_back(std::move(scene));
    }

    SE::ScriptSystem scripts;
    const bool scriptsRunning = StartHeadlessScripts(scripts, project.project);
    if (!scriptsRunning)
        ++report.errors;
    ScriptChecker checker(scripts, *context.log, report);

    // (b) every non-test script loads
    if (scriptsRunning)
    {
        for (const std::string& path : ListFilesRecursive(k_scriptsDirectory))
        {
            if (!EndsWith(path, ".lua") || IsTestScript(path))
                continue;
            if (components.count(path) != 0)
            {
                checker.Describe(path);
                continue;
            }
            const std::string module = ModuleName(path);
            if (module.empty())
            {
                PrintDiagnostic(Severity::Warning, path,
                                "not checked: the file name cannot be required as a module (it contains '.')");
                ++report.warnings;
                continue;
            }
            checker.Require(path, module);
        }
        // Components that live outside Assets/Scripts (missing ones are
        // reported by project.json or the scene that references them).
        for (const std::string& path : components)
            if (!StartsWith(path, std::string(k_scriptsDirectory) + "/") && IsRegularFile(path))
                checker.Describe(path);
    }

    // (c) scenes
    std::set<std::string> referencedScenes;
    for (const SceneCheck& scene : scenes)
    {
        if (!scene.exists)
        {
            PrintDiagnostic(Severity::Error, scene.path, "scene file does not exist");
            ++report.errors;
            continue;
        }
        for (const SE::LevelDiagnostic& diagnostic : scene.result.diagnostics)
        {
            const bool isError = diagnostic.severity == SE::LevelDiagnosticSeverity::Error;
            PrintDiagnostic(isError ? Severity::Error : Severity::Warning, scene.path,
                            diagnostic.path.empty() ? diagnostic.message
                                                    : diagnostic.path + ": " + diagnostic.message);
            ++(isError ? report.errors : report.warnings);
        }
        // Entity indices match the JSON only when every entity parsed; the
        // loader errors above already fail the check otherwise.
        if (scriptsRunning && scene.result.Succeeded())
            CheckEntityScripts(scene, checker, report, referencedScenes);
    }
    for (const auto& entry : checker.Descriptions())
        for (const SE::ScriptPropertyInfo& property : entry.second.properties)
            if (property.type == "scene" && property.defaultValue.is_string() &&
                !property.defaultValue.get<std::string>().empty())
                referencedScenes.insert(NormalizePath(property.defaultValue.get<std::string>()));
    report.referencedScenes.assign(referencedScenes.begin(), referencedScenes.end());
    scripts.Stop();

    // (d) generated API reference
    const ApiStubState stubs = CheckApiStubs();
    if (stubs != ApiStubState::UpToDate)
    {
        PrintDiagnostic(Severity::Warning, SE::ScriptSystem::k_apiStubPath,
                        stubs == ApiStubState::Missing ? "API stubs are missing; run 'FoxProject api'"
                                                       : "API stubs are stale; run 'FoxProject api'");
        ++report.warnings;
    }

    std::printf("check: %zu error(s), %zu warning(s)\n", report.errors, report.warnings);
    return report;
}

int CommandCheck(ToolContext& context, const std::vector<std::string>& args)
{
    if (!args.empty())
        return UsageError("check", "unexpected argument '" + args.front() + "'");
    return RunCheck(context).errors > 0 ? k_exitFailure : k_exitSuccess;
}

} // namespace FoxProject
