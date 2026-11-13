#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Scene/ActorTypes.h"
#include "Engine/Scene/LevelTypes.h"
#include "Engine/Scene/SceneDescriptor.h"
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

    // An actor class's components from its cached description; the editor
    // installs this as the edit scene's actorClassResolver.
    SE::ResolvedActorClass ResolveActorClass(const std::string& path);
    // ScriptFiles that declare fox.actor classes (Add > Actor).
    std::vector<std::string> ActorClasses();
    // Rebuilds placed actors whose class components changed since they were
    // last seen (the class file was edited). Call each edit-mode frame.
    void RefreshActorPreviews(SE::SceneRuntime& scene);

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
    std::unordered_map<std::string, nlohmann::json> m_previewedComponents; // by class path
    std::vector<std::string> m_files;
    std::vector<std::string> m_scenes;
};

// Draws the Scripts section of one actor: a header per script with open
// and remove buttons, its property widgets (overrides of the declared
// defaults) and an "Add Script" picker. Returns true when anything changed.
bool DrawScriptComponents(std::vector<SE::LevelScriptDefinition>& scripts,
                          ScriptCatalog& catalog, const SE::SceneRuntime& scene);

// Property overrides of an actor placement against its class's declared
// properties. Returns true when anything changed.
bool DrawActorClassProperties(SE::SceneDescriptor::ActorEntry& actor, ScriptCatalog& catalog,
                              const SE::SceneRuntime& scene);

// Component overrides of an actor placement: one section per class component
// with its fields (class values until overridden; reset restores them).
// Edits change actor.components; returns true when anything changed.
// When a position/rotation/scale changed, transformEdit receives the part
// index and its new local transform so the caller can preview it before the
// (deferred) rebuild.
struct ComponentTransformEdit
{
    int                  part = -1;
    std::array<float, 3> position{}, rotation{}, scale{};
};
bool DrawActorComponents(SE::SceneDescriptor::ActorEntry& actor, ScriptCatalog& catalog,
                         const SE::SceneRuntime& scene, ComponentTransformEdit& transformEdit);

} // namespace Editor
