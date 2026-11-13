#include "Engine/Core/Project.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Engine/Input/KeyNames.h"
#include "Engine/Scene/SceneLoader.h"

namespace SE {

namespace {

using json = nlohmann::json;

std::string NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool EndsWith(const std::string& text, const char* suffix)
{
    const size_t length = std::char_traits<char>::length(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

struct Parser
{
    std::string source;
    ProjectLoadResult result;

    void Error(const std::string& where, const std::string& message)
    {
        result.errors.push_back(source + ": " + (where.empty() ? "" : where + ": ") + message);
    }

    bool String(const json& value, const std::string& where, std::string& out)
    {
        if (!value.is_string())
        {
            Error(where, "expected a string");
            return false;
        }
        out = value.get<std::string>();
        return true;
    }

    // A project-relative file that must exist, with the given extension.
    bool File(const json& value, const std::string& where, const char* extension, std::string& out)
    {
        if (!String(value, where, out))
            return false;
        out = NormalizePath(out);
        if (!EndsWith(out, extension))
        {
            Error(where, "'" + out + "' must be a " + extension + " file");
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(out, ec))
        {
            Error(where, "'" + out + "' does not exist (paths are relative to the project root)");
            return false;
        }
        return true;
    }

    void FileList(const json& value, const std::string& where, const char* extension,
                  std::vector<std::string>& out)
    {
        if (!value.is_array())
        {
            Error(where, "expected an array of paths");
            return;
        }
        for (size_t i = 0; i < value.size(); ++i)
        {
            std::string path;
            if (File(value[i], where + "[" + std::to_string(i + 1) + "]", extension, path))
                out.push_back(std::move(path));
        }
    }

    void Input(const json& value)
    {
        if (!value.is_object())
        {
            Error("input", "expected an object of action -> key names");
            return;
        }
        for (const auto& [action, keys] : value.items())
        {
            const std::string where = "input." + action;
            if (action.empty())
            {
                Error("input", "action names must not be empty");
                continue;
            }
            std::vector<std::string> names;
            const json list = keys.is_string() ? json::array({ keys }) : keys;
            if (!list.is_array() || list.empty())
            {
                Error(where, "expected a key name or a non-empty array of key names");
                continue;
            }
            for (size_t i = 0; i < list.size(); ++i)
            {
                std::string name;
                if (!String(list[i], where + "[" + std::to_string(i + 1) + "]", name))
                    continue;
                KeyCode code;
                if (!ParseKeyName(name, code))
                {
                    Error(where + "[" + std::to_string(i + 1) + "]",
                          "unknown key name '" + name + "' (see fox.Key in Engine/Scripts/types/fox.lua)");
                    continue;
                }
                names.push_back(std::move(name));
            }
            result.project.inputActions.emplace_back(action, std::move(names));
        }
    }

    void Window(const json& value)
    {
        if (!value.is_object())
        {
            Error("window", "expected an object");
            return;
        }
        ProjectWindowSettings& window = result.project.window;
        for (const auto& [key, field] : value.items())
        {
            const std::string where = "window." + key;
            if (key == "title")
            {
                String(field, where, window.title);
            }
            else if (key == "width" || key == "height")
            {
                if (!field.is_number_integer() || field.get<int64_t>() < 64 || field.get<int64_t>() > 16384)
                {
                    Error(where, "expected an integer between 64 and 16384");
                    continue;
                }
                (key == "width" ? window.width : window.height) = field.get<uint32_t>();
            }
            else if (key == "fullscreen")
            {
                if (!field.is_boolean())
                    Error(where, "expected true or false");
                else
                    window.fullscreen = field.get<bool>();
            }
            else
            {
                Error(where, "unknown key (allowed: title, width, height, fullscreen)");
            }
        }
    }

    void Root(const json& root)
    {
        if (!root.is_object())
        {
            Error({}, "the project file must contain a JSON object");
            return;
        }
        ProjectSettings& project = result.project;
        for (const auto& [key, value] : root.items())
        {
            if (key == "name")
            {
                if (String(value, key, project.name) && project.name.empty())
                    Error(key, "must not be empty");
            }
            else if (key == "startupScene")
                File(value, key, ".json", project.startupScene);
            else if (key == "scenes")
                FileList(value, key, ".json", project.scenes);
            else if (key == "autoload")
                FileList(value, key, ".lua", project.autoloads);
            else if (key == "input")
                Input(value);
            else if (key == "window")
                Window(value);
            else if (key == "published")
            {
                if (!value.is_boolean())
                    Error(key, "expected true or false");
                else
                    project.published = value.get<bool>();
            }
            else
                Error(key, "unknown key (allowed: name, startupScene, scenes, autoload, input, window, published)");
        }
        if (project.window.title.empty())
            project.window.title = project.name;
    }
};

} // namespace

ProjectLoadResult Project::Parse(const json& root, const std::string& sourceName)
{
    Parser parser;
    parser.source = sourceName;
    parser.Root(root);
    return std::move(parser.result);
}

ProjectLoadResult Project::Load(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        ProjectLoadResult result;
        result.errors.push_back(path + ": cannot open the project file (run from the project root)");
        return result;
    }
    std::stringstream text;
    text << file.rdbuf();

    try
    {
        return Parse(json::parse(text.str(), nullptr, /*allow_exceptions*/ true, /*ignore_comments*/ true), path);
    }
    catch (const json::parse_error& error)
    {
        ProjectLoadResult result;
        result.errors.push_back(path + ": invalid JSON: " + error.what());
        return result;
    }
}

std::vector<std::string> Project::SceneList(const ProjectSettings& project)
{
    std::vector<std::string> scenes = project.scenes;
    if (scenes.empty())
        for (const std::string& scene : SceneLoader::ScanSceneDirectory("Assets/Scenes"))
            scenes.push_back(NormalizePath(scene));
    if (!project.startupScene.empty() &&
        std::find(scenes.begin(), scenes.end(), project.startupScene) == scenes.end())
        scenes.insert(scenes.begin(), project.startupScene);
    return scenes;
}

} // namespace SE
