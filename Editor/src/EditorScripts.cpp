#include "EditorScripts.h"
#include "EditorIcons.h"

#include "Engine/Core/Logger.h"
#include "Engine/Core/Project.h"
#include "Engine/Scene/SceneRuntime.h"

#include <windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>
#include <unordered_set>

namespace Editor {

namespace {

constexpr int    k_describeRefreshFrames = 30;
constexpr ImVec4 k_errorColor = { 1.0f, 0.35f, 0.35f, 1.0f };

bool EndsWith(const std::string& text, const char* suffix)
{
    const size_t n = strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

std::string FileName(const std::string& path)
{
    const size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

void OpenInShell(const std::string& path)
{
    std::error_code ec;
    const std::string absolute = std::filesystem::absolute(path, ec).string();
    const std::string& target = ec ? path : absolute;
    const int length = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
    if (length <= 0)
        return;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wide.data(), length);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        SE_LOG_ERROR("Could not open '%s' (ShellExecute error %d)", path.c_str(),
                     static_cast<int>(reinterpret_cast<INT_PTR>(result)));
}

// --- JSON value helpers ------------------------------------------------------

float ReadFloat(const nlohmann::json& value, float fallback = 0.0f)
{
    return value.is_number() ? static_cast<float>(value.get<double>()) : fallback;
}

int ReadInt(const nlohmann::json& value)
{
    return value.is_number() ? static_cast<int>(value.get<double>()) : 0;
}

uint64_t ReadEntity(const nlohmann::json& value)
{
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer())
    {
        const int64_t id = value.get<int64_t>();
        return id > 0 ? static_cast<uint64_t>(id) : 0;
    }
    return 0;
}

void ReadVec3(const nlohmann::json& value, float out[3], float fallback)
{
    for (int i = 0; i < 3; ++i)
        out[i] = (value.is_array() && value.size() == 3)
            ? ReadFloat(value[static_cast<size_t>(i)], fallback) : fallback;
}

std::string ReadString(const nlohmann::json& value)
{
    return value.is_string() ? value.get<std::string>() : std::string();
}

// Widgets edit in float precision, so compare floats the same way; an edit
// that lands on the default then removes the override.
bool SameValue(const SE::ScriptPropertyInfo& prop, const nlohmann::json& a,
               const nlohmann::json& b)
{
    if (prop.type == "number")
        return a.is_number() && b.is_number() && ReadFloat(a) == ReadFloat(b);
    if (prop.type == "integer")
        return a.is_number() && b.is_number() && ReadInt(a) == ReadInt(b);
    if (prop.type == "entity")
        return ReadEntity(a) == ReadEntity(b);
    if (prop.type == "vec3" || prop.type == "color")
    {
        if (!a.is_array() || !b.is_array()) return a == b;
        float va[3], vb[3];
        ReadVec3(a, va, 0.0f);
        ReadVec3(b, vb, 0.0f);
        return va[0] == vb[0] && va[1] == vb[1] && va[2] == vb[2];
    }
    return a == b;
}

void SetOverride(nlohmann::json& overrides, const SE::ScriptPropertyInfo& prop, nlohmann::json value)
{
    if (SameValue(prop, value, prop.defaultValue))
        overrides.erase(prop.name);
    else
        overrides[prop.name] = std::move(value);
}

// --- Entity references ---------------------------------------------------------

struct EntityChoice
{
    uint64_t    id = 0;
    std::string label;
};

void AddChoice(std::vector<EntityChoice>& out, uint64_t id, const std::string& name,
               const char* fallback)
{
    if (id == 0)
        return;
    char label[256];
    snprintf(label, sizeof(label), "%s  (#%llu)", name.empty() ? fallback : name.c_str(),
              static_cast<unsigned long long>(id));
    out.push_back({ id, label });
}

// Every scene entity that already has a level id (new entities get theirs
// when the scene is saved).
std::vector<EntityChoice> CollectEntities(const SE::SceneRuntime& scene)
{
    std::vector<EntityChoice> out;
    const auto& desc = scene.currentDesc;
    AddChoice(out, desc.camera.sourceEntityID, desc.camera.name, "Camera");
    AddChoice(out, desc.sun.sourceEntityID, desc.sun.name, "Sun");
    for (const auto& light : scene.pointLightDescs)
        AddChoice(out, light.sourceEntityID, light.name, "Point Light");
    for (const auto& mesh : scene.loadedMeshes)
        AddChoice(out, mesh.def.sourceEntityID, mesh.def.name, "Mesh");
    for (const auto& object : scene.sceneObjects)
        AddChoice(out, object.def.sourceEntityID, object.def.name, "Object");
    for (const auto& emitter : scene.particleEmitterDescs)
        AddChoice(out, emitter.sourceEntityID, emitter.name, "Emitter");
    for (const auto& empty : scene.emptyEntities)
        AddChoice(out, empty.def.sourceEntityID, empty.def.name, "Empty");
    for (const auto& actor : scene.actors)
        if (!actor.spawned)
            AddChoice(out, actor.def.sourceEntityID, actor.def.name, "Actor");
    return out;
}

// --- Property widgets ------------------------------------------------------------

// Draws the widget for prop's type; on change stores the new JSON value in edited.
bool DrawPropertyWidget(const SE::ScriptPropertyInfo& prop, const nlohmann::json& current,
                        nlohmann::json& edited, ScriptCatalog& catalog,
                        const SE::SceneRuntime& scene)
{
    const char* label = prop.name.c_str();
    const std::string& type = prop.type;
    if (type == "number")
    {
        float v = ReadFloat(current);
        const float speed = prop.step ? static_cast<float>(*prop.step) : 0.1f;
        bool changed;
        if (prop.min && prop.max)
            changed = ImGui::SliderFloat(label, &v, static_cast<float>(*prop.min),
                                         static_cast<float>(*prop.max));
        else
        {
            // A single bound clamps one side; no bounds leaves the drag unclamped.
            const bool bounded = prop.min || prop.max;
            const float lo = prop.min ? static_cast<float>(*prop.min) : (bounded ? -FLT_MAX : 0.0f);
            const float hi = prop.max ? static_cast<float>(*prop.max) : (bounded ? FLT_MAX : 0.0f);
            changed = ImGui::DragFloat(label, &v, speed, lo, hi);
        }
        if (changed) edited = static_cast<double>(v);
        return changed;
    }
    if (type == "integer")
    {
        int v = ReadInt(current);
        const float speed = prop.step ? static_cast<float>(*prop.step) : 0.2f;
        const bool bounded = prop.min || prop.max;
        const int lo = prop.min ? static_cast<int>(*prop.min) : (bounded ? INT_MIN : 0);
        const int hi = prop.max ? static_cast<int>(*prop.max) : (bounded ? INT_MAX : 0);
        const bool changed = ImGui::DragInt(label, &v, speed, lo, hi);
        if (changed) edited = v;
        return changed;
    }
    if (type == "boolean")
    {
        bool v = current.is_boolean() && current.get<bool>();
        const bool changed = ImGui::Checkbox(label, &v);
        if (changed) edited = v;
        return changed;
    }
    if (type == "string")
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", ReadString(current).c_str());
        const bool changed = ImGui::InputText(label, buf, sizeof(buf));
        if (changed) edited = std::string(buf);
        return changed;
    }
    if (type == "vec3" || type == "color")
    {
        float v[3];
        ReadVec3(current, v, 0.0f);
        const float speed = prop.step ? static_cast<float>(*prop.step) : 0.1f;
        // Linear colors (light colors especially) go above 1: edit them as floats.
        const bool changed = type == "color"
            ? ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
            : ImGui::DragFloat3(label, v, speed);
        if (changed) edited = nlohmann::json::array({ v[0], v[1], v[2] });
        return changed;
    }
    if (type == "enum")
    {
        const std::string value = ReadString(current);
        bool changed = false;
        if (ImGui::BeginCombo(label, value.c_str()))
        {
            for (const std::string& option : prop.options)
                if (ImGui::Selectable(option.c_str(), option == value))
                {
                    edited = option;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }
    if (type == "entity")
    {
        const uint64_t id = ReadEntity(current);
        const std::vector<EntityChoice> choices = CollectEntities(scene);
        std::string preview = "None";
        if (id != 0)
        {
            char missing[64];
            sprintf_s(missing, "Missing (#%llu)", static_cast<unsigned long long>(id));
            preview = missing;
            for (const auto& choice : choices)
                if (choice.id == id) preview = choice.label;
        }
        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable("None", id == 0))
            {
                edited = nullptr;
                changed = true;
            }
            for (const auto& choice : choices)
            {
                ImGui::PushID(static_cast<int>(choice.id & 0x7fffffff));
                if (ImGui::Selectable(choice.label.c_str(), choice.id == id))
                {
                    edited = choice.id;
                    changed = true;
                }
                ImGui::PopID();
            }
            if (choices.empty())
                ImGui::TextDisabled("Save the scene to give new entities ids.");
            ImGui::EndCombo();
        }
        return changed;
    }
    if (type == "scene")
    {
        const std::string value = ReadString(current);
        bool changed = false;
        if (ImGui::BeginCombo(label, value.empty() ? "None" : value.c_str()))
        {
            if (ImGui::IsWindowAppearing())
                catalog.ReloadScenes();
            for (const std::string& path : catalog.Scenes())
                if (ImGui::Selectable(path.c_str(), path == value))
                {
                    edited = path;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }
    ImGui::TextDisabled("%s: unsupported type '%s'", label, type.c_str());
    return false;
}

float ResetButtonWidth()
{
    return ImGui::CalcTextSize(ICON_MD_REFRESH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Reset button (or a same-sized spacer) in front of a property row.
bool DrawResetSlot(bool overridden)
{
    if (!overridden)
    {
        ImGui::Dummy({ ResetButtonWidth(), ImGui::GetTextLineHeight() });
        ImGui::SameLine();
        return false;
    }
    const bool reset = ImGui::SmallButton(ICON_MD_REFRESH "##reset");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset to the script default");
    ImGui::SameLine();
    return reset;
}

bool DrawScriptProperties(SE::LevelScriptDefinition& def, const SE::ScriptDescription& desc,
                          ScriptCatalog& catalog, const SE::SceneRuntime& scene)
{
    if (!def.properties.is_object())
        def.properties = nlohmann::json::object();
    bool changed = false;
    for (const SE::ScriptPropertyInfo& prop : desc.properties)
    {
        ImGui::PushID(prop.name.c_str());
        const auto it = def.properties.find(prop.name);
        const bool overridden = it != def.properties.end();
        if (DrawResetSlot(overridden))
        {
            def.properties.erase(prop.name);
            changed = true;
            ImGui::PopID();
            continue;
        }
        const nlohmann::json current = overridden ? *it : prop.defaultValue;
        nlohmann::json edited;
        if (DrawPropertyWidget(prop, current, edited, catalog, scene))
        {
            SetOverride(def.properties, prop, std::move(edited));
            changed = true;
        }
        if (!prop.tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", prop.tooltip.c_str());
        ImGui::PopID();
    }

    // Overrides the script no longer declares: shown so they can be removed.
    if (!desc.ok)
        return changed;
    std::vector<std::string> stale;
    for (auto it = def.properties.begin(); it != def.properties.end(); ++it)
    {
        const bool declared = std::any_of(desc.properties.begin(), desc.properties.end(),
            [&](const SE::ScriptPropertyInfo& p) { return p.name == it.key(); });
        if (!declared)
            stale.push_back(it.key());
    }
    for (const std::string& key : stale)
    {
        ImGui::PushID(key.c_str());
        if (DrawResetSlot(true))
        {
            def.properties.erase(key);
            changed = true;
        }
        else
            ImGui::TextDisabled("%s = %s (not declared)", key.c_str(),
                                def.properties[key].dump().c_str());
        ImGui::PopID();
    }
    return changed;
}

// Header row: file name, then open and remove buttons right-aligned.
// Returns true when the script should be removed.
bool DrawScriptHeader(const SE::LevelScriptDefinition& def, bool& open)
{
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const std::string title = std::string(ICON_MD_CODE " ") + FileName(def.script);
    open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                  ImGuiTreeNodeFlags_AllowOverlap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", def.script.c_str());
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonW = ImGui::CalcTextSize(ICON_MD_OPEN_IN_NEW).x + style.FramePadding.x * 2.0f;
    ImGui::SameLine(right - buttonW * 2.0f - style.ItemSpacing.x);
    if (ImGui::SmallButton(ICON_MD_OPEN_IN_NEW "##open"))
        OpenInShell(def.script);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open in the default editor");
    ImGui::SameLine();
    const bool remove = ImGui::SmallButton(ICON_MD_CLOSE "##remove");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Remove script");
    return remove;
}

// "Add Script" picker over every describable script in Assets/Scripts.
bool DrawAddScriptPopup(std::vector<SE::LevelScriptDefinition>& scripts,
                        ScriptCatalog& catalog)
{
    static ImGuiTextFilter s_filter;
    if (ImGui::Button("Add Script", { -FLT_MIN, 0.0f }))
    {
        catalog.RescanScriptFiles();
        s_filter.Clear();
        ImGui::OpenPopup("##add_script");
    }
    if (!ImGui::BeginPopup("##add_script"))
        return false;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::InputTextWithHint("##filter", "Filter scripts...", s_filter.InputBuf,
                                 IM_ARRAYSIZE(s_filter.InputBuf)))
        s_filter.Build();
    bool added = false;
    bool any = false;
    for (const std::string& path : catalog.ScriptFiles())
    {
        // Actor classes are placed (Add > Actor), not attached.
        if (!s_filter.PassFilter(path.c_str()))
            continue;
        const SE::ScriptDescription& desc = catalog.Describe(path);
        if (!desc.ok || desc.actor)
            continue;
        any = true;
        if (ImGui::Selectable(path.c_str()))
        {
            scripts.push_back({ path, nlohmann::json::object() });
            added = true;
            ImGui::CloseCurrentPopup();
            break;
        }
    }
    if (!any)
        ImGui::TextDisabled("No loadable scripts in Assets/Scripts");
    ImGui::EndPopup();
    return added;
}

// --- Actor components ------------------------------------------------------------

SE::ScriptPropertyInfo Field(const char* name, const char* type, const char* tooltip,
                             std::optional<double> min = {}, std::optional<double> max = {})
{
    SE::ScriptPropertyInfo info;
    info.name = name;
    info.type = type;
    info.tooltip = tooltip;
    info.min = min;
    info.max = max;
    return info;
}

// The overridable keys of a component type (ParseActorComponent's keys), as
// property schemas so the script property widgets can edit them.
std::vector<SE::ScriptPropertyInfo> ComponentFields(SE::ActorComponentType type,
                                                    const SE::SceneRuntime& scene)
{
    using Type = SE::ActorComponentType;
    std::vector<SE::ScriptPropertyInfo> fields = {
        Field("position", "vec3", "Relative to the actor."),
        Field("rotation", "vec3", "Euler degrees, relative to the actor."),
        Field("scale", "vec3", "Relative to the actor."),
    };
    auto body = [&fields] {
        SE::ScriptPropertyInfo kind = Field("body", "enum",
            "kinematic follows the actor; dynamic is simulated and moves the actor.");
        kind.options = { "kinematic", "dynamic" };
        fields.push_back(kind);
        fields.push_back(Field("mass", "number", "Dynamic body mass in kg.", 0.001, {}));
        fields.push_back(Field("friction", "number", "", 0.0, {}));
        fields.push_back(Field("restitution", "number", "Bounciness.", 0.0, 1.0));
    };
    switch (type)
    {
    case Type::Mesh:
        fields.push_back(Field("mesh", "string", "Model path (FBX, glTF, OBJ)."));
        fields.push_back(Field("cast_shadows", "boolean", ""));
        fields.push_back(Field("receive_shadows", "boolean", ""));
        fields.push_back(Field("alpha_cutoff", "number", "Alpha-test threshold for textures with alpha (0 = off).",
                               0.0, 1.0));
        fields.push_back(Field("collision", "boolean", "Kinematic triangle collider that follows the mesh."));
        break;
    case Type::PointLight:
        fields.push_back(Field("color", "color", "Linear RGB."));
        fields.push_back(Field("intensity", "number", "", 0.0, {}));
        fields.push_back(Field("radius", "number", "Range in m.", 0.01, {}));
        break;
    case Type::ParticleEmitter:
    {
        SE::ScriptPropertyInfo preset = Field("preset", "enum", "Assets/Particles/Presets/*.json");
        for (std::string path : scene.particlePresetFiles)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            preset.options.push_back(std::move(path));
        }
        fields.push_back(std::move(preset));
        break;
    }
    case Type::Box:
        fields.push_back(Field("size", "vec3", "Full extents."));
        body();
        break;
    case Type::Sphere:
        fields.push_back(Field("radius", "number", "", 0.001, {}));
        body();
        break;
    }
    return fields;
}

nlohmann::json ComponentValue(const SE::ActorComponentDefinition& d, const std::string& key)
{
    auto vec = [](const std::array<float, 3>& v) { return nlohmann::json::array({ v[0], v[1], v[2] }); };
    if (key == "position") return vec(d.transform.position);
    if (key == "rotation") return vec(d.transform.rotationDegrees);
    if (key == "scale") return vec(d.transform.scale);
    if (key == "mesh") return d.mesh;
    if (key == "cast_shadows") return d.castShadows;
    if (key == "receive_shadows") return d.receiveShadows;
    if (key == "alpha_cutoff") return d.alphaCutoff;
    if (key == "collision") return d.collision;
    if (key == "color") return vec(d.color);
    if (key == "intensity") return d.intensity;
    if (key == "radius") return d.radius;
    if (key == "preset") return d.preset;
    if (key == "size") return vec(d.size);
    if (key == "body") return d.body == SE::ActorBodyType::Dynamic ? "dynamic" : "kinematic";
    if (key == "mass") return d.mass;
    if (key == "friction") return d.friction;
    if (key == "restitution") return d.restitution;
    return nullptr;
}

// Effective value of key: the placement's override, else the class's.
const nlohmann::json& EffectiveValue(const nlohmann::json& overrides, const std::string& key,
                                     const nlohmann::json& classValue)
{
    const auto it = overrides.find(key);
    return it != overrides.end() ? *it : classValue;
}

// Overrides of components the class no longer declares: shown so they can be removed.
bool DrawStaleComponentOverrides(SE::SceneDescriptor::ActorEntry& actor, const nlohmann::json& classComponents)
{
    std::vector<std::string> stale;
    for (auto it = actor.components.begin(); it != actor.components.end(); ++it)
    {
        const bool declared = std::any_of(classComponents.begin(), classComponents.end(),
            [&](const nlohmann::json& c) { return c.is_object() && c.value("name", std::string()) == it.key(); });
        if (!declared)
            stale.push_back(it.key());
    }
    bool changed = false;
    for (const std::string& key : stale)
    {
        ImGui::PushID(key.c_str());
        if (DrawResetSlot(true))
        {
            actor.components.erase(key);
            changed = true;
        }
        else
            ImGui::TextDisabled("%s = %s (no such component)", key.c_str(), actor.components[key].dump().c_str());
        ImGui::PopID();
    }
    return changed;
}

} // namespace

// --- ScriptCatalog --------------------------------------------------------------

bool ScriptCatalog::Start()
{
    if (!m_session.Start(SE::ScriptHost{}))
    {
        SE_LOG_ERROR("Editor: could not start the script schema session");
        return false;
    }
    RescanScriptFiles();
    ReloadScenes();
    return true;
}

void ScriptCatalog::Stop()
{
    if (m_session.IsRunning())
        m_session.Stop();
    m_descriptions.clear();
}

const SE::ScriptDescription& ScriptCatalog::Describe(const std::string& path)
{
    const int frame = ImGui::GetFrameCount();
    auto it = m_descriptions.find(path);
    if (it != m_descriptions.end() && frame - it->second.frame < k_describeRefreshFrames &&
        frame >= it->second.frame)
        return it->second.description;

    CachedDescription& cached = m_descriptions[path];
    cached.frame = frame;
    if (m_session.IsRunning())
        cached.description = m_session.Describe(path);
    else
    {
        cached.description = {};
        cached.description.error = "Script session is not running";
    }
    return cached.description;
}

SE::ResolvedActorClass ScriptCatalog::ResolveActorClass(const std::string& path)
{
    const SE::ScriptDescription& desc = Describe(path);
    SE::ResolvedActorClass result;
    if (!desc.ok)
        result.error = desc.error;
    else if (!desc.actor)
        result.error = path + " is not an actor class: declare it with fox.actor { components = { ... } }";
    else
        result.components = desc.components;
    return result;
}

std::vector<std::string> ScriptCatalog::ActorClasses()
{
    std::vector<std::string> classes;
    for (const std::string& path : m_files)
    {
        const SE::ScriptDescription& desc = Describe(path);
        if (desc.ok && desc.actor)
            classes.push_back(path);
    }
    return classes;
}

void ScriptCatalog::RefreshActorPreviews(SE::SceneRuntime& scene)
{
    // A class is compared once per frame; a broken save keeps the last preview.
    std::unordered_set<std::string> changed;
    std::unordered_set<std::string> seen;
    for (const auto& actor : scene.actors)
    {
        const std::string& path = actor.def.actorClass;
        if (!seen.insert(path).second)
            continue;
        const SE::ScriptDescription& desc = Describe(path);
        if (!desc.ok || !desc.actor)
            continue;
        const auto [it, first] = m_previewedComponents.try_emplace(path, desc.components);
        if (first)
        {
            // The class failed when the level loaded and loads now.
            if (!actor.def.resolveError.empty())
                changed.insert(path);
        }
        else if (it->second != desc.components)
        {
            it->second = desc.components;
            changed.insert(path);
        }
    }
    if (changed.empty())
        return;
    for (int i = 0; i < static_cast<int>(scene.actors.size()); ++i)
        if (!scene.actors[i].spawned && changed.count(scene.actors[i].def.actorClass))
            scene.RebuildActor(i);
}

void ScriptCatalog::RescanScriptFiles()
{
    m_files.clear();
    std::error_code ec;
    const std::filesystem::path root = "Assets/Scripts";
    if (!std::filesystem::is_directory(root, ec))
        return;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string path = it->path().generic_string();
        if (EndsWith(path, ".lua") && !EndsWith(path, "_test.lua"))
            m_files.push_back(path);
    }
    std::sort(m_files.begin(), m_files.end());
}

void ScriptCatalog::ReloadScenes()
{
    const SE::ProjectLoadResult loaded = SE::Project::Load();
    m_scenes = SE::Project::SceneList(loaded.project);
}

// --- Details section -------------------------------------------------------------

bool DrawScriptComponents(std::vector<SE::LevelScriptDefinition>& scripts,
                          ScriptCatalog& catalog, const SE::SceneRuntime& scene)
{
    ImGui::SeparatorText("Scripts");
    bool changed = false;
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(scripts.size()); ++i)
    {
        SE::LevelScriptDefinition& def = scripts[static_cast<size_t>(i)];
        ImGui::PushID(i);
        bool open = false;
        if (DrawScriptHeader(def, open))
            removeIndex = i;
        if (open)
        {
            const SE::ScriptDescription& desc = catalog.Describe(def.script);
            if (!desc.ok)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(k_errorColor, "%s", desc.error.c_str());
                ImGui::PopTextWrapPos();
            }
            changed |= DrawScriptProperties(def, desc, catalog, scene);
            if (desc.ok && desc.properties.empty())
                ImGui::TextDisabled("No properties");
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0)
    {
        scripts.erase(scripts.begin() + removeIndex);
        changed = true;
    }
    changed |= DrawAddScriptPopup(scripts, catalog);
    return changed;
}

bool DrawActorClassProperties(SE::SceneDescriptor::ActorEntry& actor, ScriptCatalog& catalog,
                              const SE::SceneRuntime& scene)
{
    ImGui::SeparatorText("Properties");
    const SE::ScriptDescription& desc = catalog.Describe(actor.actorClass);
    if (!desc.ok)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(k_errorColor, "%s", desc.error.c_str());
        ImGui::PopTextWrapPos();
    }
    SE::LevelScriptDefinition def{ actor.actorClass, actor.properties };
    const bool changed = DrawScriptProperties(def, desc, catalog, scene);
    if (changed)
        actor.properties = std::move(def.properties);
    if (desc.ok && desc.properties.empty())
        ImGui::TextDisabled("No properties");
    return changed;
}

bool DrawActorComponents(SE::SceneDescriptor::ActorEntry& actor, ScriptCatalog& catalog,
                         const SE::SceneRuntime& scene, ComponentTransformEdit& transformEdit)
{
    ImGui::SeparatorText("Components");
    const SE::ResolvedActorClass cls = catalog.ResolveActorClass(actor.actorClass);
    if (!cls.error.empty())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(k_errorColor, "%s", cls.error.c_str());
        ImGui::PopTextWrapPos();
        return false;
    }
    if (!actor.components.is_object())
        actor.components = nlohmann::json::object();

    bool changed = false;
    for (size_t i = 0; i < cls.components.size(); ++i)
    {
        // The class was validated when it loaded (fox.actor checks every component).
        SE::ActorComponentDefinition classDef;
        std::string error;
        if (!SE::ParseActorComponent(cls.components[i], classDef, error))
            continue;
        ImGui::PushID(static_cast<int>(i));
        const std::string title = classDef.name + "  (" + SE::ActorComponentTypeName(classDef.type) + ")";
        const auto existing = actor.components.find(classDef.name);
        nlohmann::json overrides = existing != actor.components.end() && existing->is_object()
            ? *existing : nlohmann::json::object();
        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool edited = false;
            bool transformChanged = false;
            for (SE::ScriptPropertyInfo& field : ComponentFields(classDef.type, scene))
            {
                ImGui::PushID(field.name.c_str());
                field.defaultValue = ComponentValue(classDef, field.name);
                const bool overridden = overrides.contains(field.name);
                if (DrawResetSlot(overridden))
                {
                    overrides.erase(field.name);
                    edited = true;
                    transformChanged |= field.name == "position" || field.name == "rotation" || field.name == "scale";
                    ImGui::PopID();
                    continue;
                }
                nlohmann::json value;
                if (DrawPropertyWidget(field, EffectiveValue(overrides, field.name, field.defaultValue),
                                       value, catalog, scene))
                {
                    SetOverride(overrides, field, std::move(value));
                    edited = true;
                    transformChanged |= field.name == "position" || field.name == "rotation" || field.name == "scale";
                }
                if (!field.tooltip.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", field.tooltip.c_str());
                ImGui::PopID();
            }
            if (edited)
            {
                if (overrides.empty())
                    actor.components.erase(classDef.name);
                else
                    actor.components[classDef.name] = overrides;
                changed = true;
            }
            if (transformChanged)
            {
                float v[3];
                transformEdit.part = static_cast<int>(i);
                ReadVec3(EffectiveValue(overrides, "position", ComponentValue(classDef, "position")), v, 0.0f);
                transformEdit.position = { v[0], v[1], v[2] };
                ReadVec3(EffectiveValue(overrides, "rotation", ComponentValue(classDef, "rotation")), v, 0.0f);
                transformEdit.rotation = { v[0], v[1], v[2] };
                ReadVec3(EffectiveValue(overrides, "scale", ComponentValue(classDef, "scale")), v, 1.0f);
                transformEdit.scale = { v[0], v[1], v[2] };
            }
        }
        ImGui::PopID();
    }
    changed |= DrawStaleComponentOverrides(actor, cls.components);
    if (cls.components.empty())
        ImGui::TextDisabled("No components");
    return changed;
}

} // namespace Editor
