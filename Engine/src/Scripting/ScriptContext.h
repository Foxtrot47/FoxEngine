#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include "Engine/Input/ActionMap.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Scripting/Sol.h"
#include "Scripting/ScriptTypes.h"

struct ImDrawList;

namespace SE::Scripting {

class ScriptApi;

// Script-instance services the ScriptSystem provides to bindings.
class ScriptInstances
{
public:
    // The instance of the script at path on the owner entity (0: autoloads), or nil.
    virtual sol::object FindScript(sol::state_view lua, uint32_t owner, const std::string& path) = 0;
    // Instantiates a script component on the owner entity (properties: overrides
    // table or nil). Raises a ScriptError when the script cannot be loaded.
    virtual sol::object AddScript(sol::state_view lua, uint32_t owner, const std::string& path,
                                  const sol::object& properties) = 0;

    // fox.world: creates an actor (class table or path; options table or nil)
    // and returns its class instance. Raises when the class cannot be used.
    virtual sol::object SpawnActor(sol::state_view lua, const sol::object& actorClass,
                                   const sol::object& options) = 0;
    // Destroys the actor at the end of the frame (on_destroy runs then).
    virtual void DestroyActor(uint32_t root) = 0;
    // The class instance of the actor whose root entity is root, or nil.
    virtual sol::object ActorInstance(sol::state_view lua, uint32_t root) = 0;
    // Every actor's class instance in creation order.
    virtual sol::table ActorInstances(sol::state_view lua) = 0;

protected:
    ~ScriptInstances() = default;
};

// State shared by the ScriptSystem and the binding files. Owned by the
// ScriptSystem; bindings capture a reference to it.
struct ScriptContext
{
    ScriptHost host;
    bool headless = false;   // registering for API stub generation only
    ScriptInstances* instances = nullptr;

    // fox.time
    double dt = 0.0;
    double unscaledDt = 0.0;
    double time = 0.0;
    double realTime = 0.0;
    double scale = 1.0;
    uint64_t frame = 0;
    double fps = 0.0;

    // fox.input
    bool inputFocus = true;
    ActionMap actions;
    std::unordered_set<std::string> actionNames;

    // fox.scene: bumped whenever the scene changes, invalidating entity handles.
    uint32_t sceneGeneration = 1;

    // fox.ui (valid only while inGui)
    ScriptViewRect view;
    bool inGui = false;
    int uiWindowDepth = 0;
    bool uiWantsMouse = false;
    bool uiWantsKeyboard = false;
    bool uiWantsMouseNext = false;
    bool uiWantsKeyboardNext = false;
    ImDrawList* overlay = nullptr;
};

// One file per fox.* module under Bindings/; RegisterAllBindings lists them.
void BindCore(ScriptApi& api, ScriptContext& ctx);
void BindMath(ScriptApi& api, ScriptContext& ctx);
void BindInput(ScriptApi& api, ScriptContext& ctx);
void BindScene(ScriptApi& api, ScriptContext& ctx);
void BindVehicle(ScriptApi& api, ScriptContext& ctx);
void BindUi(ScriptApi& api, ScriptContext& ctx);

void RegisterAllBindings(ScriptApi& api, ScriptContext& ctx);

// fox.Entity handle for an entity of the current scene.
inline ScriptEntity MakeEntityHandle(const ScriptContext& ctx, uint32_t id)
{
    return ScriptEntity{ id, ctx.sceneGeneration };
}

} // namespace SE::Scripting
