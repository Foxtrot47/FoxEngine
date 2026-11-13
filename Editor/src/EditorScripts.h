#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Scene/LevelTypes.h"
#include "Engine/Scripting/ScriptSystem.h"

namespace SE {
class SceneRuntime;
}

namespace Editor {

// Script property schemas for the Details panel. A headless script session
// (no input, window or scene) lives for the whole editor run so describing a
// script never touches the play session.
class ScriptCatalog
{
public:
    bool Start();
    void Stop();

    // Cached per path; refreshed every few frames so edits to the file (and
    // its fixed errors) show up while the editor runs.
    const SE::ScriptDescription& Describe(const std::string& path);

    // Assets/Scripts/**/*.lua except *_test.lua ('/' separators, sorted).
    const std::vector<std::string>& ScriptFiles() const { return m_files; }
    void RescanScriptFiles();

    // Project::SceneList of project.json, reloaded on request.
    const std::vector<std::string>& Scenes() const { return m_scenes; }
    void ReloadScenes();

private:
    struct CachedDescription
    {
        SE::ScriptDescription description;
        int                   frame = 0;
    };

    SE::ScriptSystem m_session;
    std::unordered_map<std::string, CachedDescription> m_descriptions;
    std::vector<std::string> m_files;
    std::vector<std::string> m_scenes;
};

// Draws the Scripts section of one entity: a header per script with open
// and remove buttons, its property widgets (overrides of the declared
// defaults) and an "Add Script" picker. Returns true when anything changed.
bool DrawScriptComponents(std::vector<SE::LevelScriptDefinition>& scripts,
                          ScriptCatalog& catalog, const SE::SceneRuntime& scene);

} // namespace Editor
