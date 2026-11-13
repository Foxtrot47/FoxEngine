#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace SE {

struct ProjectWindowSettings
{
    std::string title;           // empty: the project name
    uint32_t    width      = 1280;
    uint32_t    height     = 720;
    bool        fullscreen = false;
};

// project.json at the project root: what the player runs and publishes.
struct ProjectSettings
{
    std::string name = "Untitled";
    std::string startupScene;                 // project-relative scene path
    std::vector<std::string> scenes;          // published scenes; empty: every scene in Assets/Scenes
    std::vector<std::string> autoloads;       // script paths instantiated in order at startup
    // Input actions for fox.input.action_*: action -> key names ("W", "Gamepad.A").
    std::vector<std::pair<std::string, std::vector<std::string>>> inputActions;
    ProjectWindowSettings window;
    bool published = false;                   // set by `FoxProject publish`
};

struct ProjectLoadResult
{
    ProjectSettings project;
    // "project.json: input.jump[1]: unknown key name 'Spcae'"
    std::vector<std::string> errors;

    bool Succeeded() const { return errors.empty(); }
};

class Project
{
public:
    static constexpr const char* k_fileName = "project.json";

    // Reads and validates project.json (paths relative to the project root,
    // which is the working directory). Unknown keys, bad key names and
    // missing scene/script files are errors.
    static ProjectLoadResult Load(const std::string& path = k_fileName);
    static ProjectLoadResult Parse(const nlohmann::json& root, const std::string& sourceName = k_fileName);

    // The scenes a published build contains: project.scenes, or every scene
    // in Assets/Scenes when the list is empty. Includes startupScene.
    static std::vector<std::string> SceneList(const ProjectSettings& project);
};

} // namespace SE
