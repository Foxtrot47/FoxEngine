#include "Engine/Scene/ActorTypes.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <unordered_set>
#include "Engine/Scene/SceneLoader.h"

namespace SE {

using json = nlohmann::json;

namespace {

struct TypeInfo
{
    ActorComponentType type;
    const char* name;
    std::initializer_list<const char*> keys; // besides name, type, position, rotation, scale
};

const TypeInfo k_types[] = {
    { ActorComponentType::Mesh, "mesh", { "mesh", "cast_shadows", "receive_shadows", "alpha_cutoff", "collision" } },
    { ActorComponentType::PointLight, "point_light", { "color", "intensity", "radius" } },
    { ActorComponentType::ParticleEmitter, "particle_emitter", { "preset" } },
    { ActorComponentType::Box, "box",
      { "size", "visible", "collision", "color", "emissive", "cast_shadows", "body", "mass", "friction",
        "restitution" } },
    { ActorComponentType::Sphere, "sphere",
      { "radius", "visible", "collision", "color", "emissive", "cast_shadows", "body", "mass", "friction",
        "restitution" } },
};

const TypeInfo* FindType(const std::string& name)
{
    for (const TypeInfo& info : k_types)
        if (name == info.name)
            return &info;
    return nullptr;
}

bool IsIdentifier(const std::string& text)
{
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'))
        return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

std::string AllowedKeys(const TypeInfo& info)
{
    std::string text = "name, type, position, rotation, scale";
    for (const char* key : info.keys)
        text += std::string(", ") + key;
    return text;
}

// Field readers: false with error set when the value has the wrong shape.
bool ReadNumber(const json& source, const char* key, float& out, std::string& error,
                float min = -1e30f, float max = 1e30f)
{
    if (!source.contains(key))
        return true;
    const json& value = source[key];
    if (!value.is_number() || !std::isfinite(value.get<double>()))
    {
        error = std::string("'") + key + "' must be a number";
        return false;
    }
    const float number = value.get<float>();
    if (number < min || number > max)
    {
        error = std::string("'") + key + "' is out of range";
        return false;
    }
    out = number;
    return true;
}

bool ReadPositive(const json& source, const char* key, float& out, std::string& error)
{
    if (!ReadNumber(source, key, out, error))
        return false;
    if (out <= 0.0f)
    {
        error = std::string("'") + key + "' must be greater than zero";
        return false;
    }
    return true;
}

bool ReadBool(const json& source, const char* key, bool& out, std::string& error)
{
    if (!source.contains(key))
        return true;
    if (!source[key].is_boolean())
    {
        error = std::string("'") + key + "' must be a boolean";
        return false;
    }
    out = source[key].get<bool>();
    return true;
}

bool ReadVec3(const json& source, const char* key, std::array<float, 3>& out, std::string& error)
{
    if (!source.contains(key))
        return true;
    const json& value = source[key];
    if (!value.is_array() || value.size() != 3 ||
        !std::all_of(value.begin(), value.end(), [](const json& v) {
            return v.is_number() && std::isfinite(v.get<double>());
        }))
    {
        error = std::string("'") + key + "' must be a vec3 ([x, y, z] in level files)";
        return false;
    }
    for (size_t i = 0; i < 3; ++i)
        out[i] = value[i].get<float>();
    return true;
}

bool ReadPath(const json& source, const char* key, std::string& out, std::string& error)
{
    if (!source.contains(key))
    {
        error = std::string("'") + key + "' is required";
        return false;
    }
    if (!source[key].is_string() || source[key].get<std::string>().empty())
    {
        error = std::string("'") + key + "' must be a non-empty path";
        return false;
    }
    out = source[key].get<std::string>();
    std::replace(out.begin(), out.end(), '\\', '/');
    return true;
}

} // namespace

const char* ActorComponentTypeName(ActorComponentType type)
{
    for (const TypeInfo& info : k_types)
        if (info.type == type)
            return info.name;
    return "unknown";
}

bool ParseActorComponent(const json& source, ActorComponentDefinition& out, std::string& error)
{
    out = ActorComponentDefinition{};
    if (!source.is_object())
    {
        error = "a component must be a table { name = ..., type = ... }";
        return false;
    }
    if (!source.contains("name") || !source["name"].is_string() ||
        !IsIdentifier(source["name"].get<std::string>()))
    {
        error = "'name' must be an identifier (it becomes self.<name> on the actor)";
        return false;
    }
    out.name = source["name"].get<std::string>();
    const std::string where = "component '" + out.name + "': ";

    const TypeInfo* info = source.contains("type") && source["type"].is_string()
        ? FindType(source["type"].get<std::string>()) : nullptr;
    if (!info)
    {
        error = where + "'type' must be one of mesh, point_light, particle_emitter, box, sphere";
        return false;
    }
    out.type = info->type;

    for (auto it = source.begin(); it != source.end(); ++it)
    {
        const std::string& key = it.key();
        const bool common = key == "name" || key == "type" || key == "position" ||
                            key == "rotation" || key == "scale";
        const bool known = common || std::any_of(info->keys.begin(), info->keys.end(),
                                                 [&](const char* k) { return key == k; });
        if (!known)
        {
            error = where + "unknown key '" + key + "' for a " + info->name + " (allowed: " +
                    AllowedKeys(*info) + ")";
            return false;
        }
    }

    std::string fieldError;
    auto fail = [&] {
        error = where + fieldError;
        return false;
    };
    if (!ReadVec3(source, "position", out.transform.position, fieldError) ||
        !ReadVec3(source, "rotation", out.transform.rotationDegrees, fieldError) ||
        !ReadVec3(source, "scale", out.transform.scale, fieldError))
        return fail();

    switch (out.type)
    {
    case ActorComponentType::Mesh:
        if (!ReadPath(source, "mesh", out.mesh, fieldError) ||
            !ReadBool(source, "cast_shadows", out.castShadows, fieldError) ||
            !ReadBool(source, "receive_shadows", out.receiveShadows, fieldError) ||
            !ReadNumber(source, "alpha_cutoff", out.alphaCutoff, fieldError, 0.0f, 1.0f) ||
            !ReadBool(source, "collision", out.collision, fieldError))
            return fail();
        break;
    case ActorComponentType::PointLight:
        out.radius = 10.0f;
        if (!ReadVec3(source, "color", out.color, fieldError) ||
            !ReadNumber(source, "intensity", out.intensity, fieldError, 0.0f) ||
            !ReadPositive(source, "radius", out.radius, fieldError))
            return fail();
        break;
    case ActorComponentType::ParticleEmitter:
        if (!ReadPath(source, "preset", out.preset, fieldError))
            return fail();
        break;
    case ActorComponentType::Box:
    case ActorComponentType::Sphere:
    {
        if (out.type == ActorComponentType::Box)
        {
            if (!ReadVec3(source, "size", out.size, fieldError))
                return fail();
            if (out.size[0] <= 0.0f || out.size[1] <= 0.0f || out.size[2] <= 0.0f)
            {
                fieldError = "'size' components must be greater than zero";
                return fail();
            }
        }
        else if (!ReadPositive(source, "radius", out.radius, fieldError))
        {
            return fail();
        }
        out.color = { 0.8f, 0.8f, 0.8f };
        out.collision = true;
        if (!ReadBool(source, "visible", out.visible, fieldError) ||
            !ReadBool(source, "collision", out.collision, fieldError) ||
            !ReadVec3(source, "color", out.color, fieldError) ||
            !ReadVec3(source, "emissive", out.emissive, fieldError) ||
            !ReadBool(source, "cast_shadows", out.castShadows, fieldError))
            return fail();
        if (std::any_of(out.color.begin(), out.color.end(), [](float c) { return c < 0.0f || c > 1.0f; }))
        {
            fieldError = "'color' components must be in 0..1 (albedo)";
            return fail();
        }
        if (std::any_of(out.emissive.begin(), out.emissive.end(), [](float c) { return c < 0.0f; }))
        {
            fieldError = "'emissive' components must not be negative";
            return fail();
        }
        if (source.contains("body"))
        {
            const json& body = source["body"];
            if (body == "kinematic")
                out.body = ActorBodyType::Kinematic;
            else if (body == "dynamic")
                out.body = ActorBodyType::Dynamic;
            else
            {
                fieldError = "'body' must be \"kinematic\" (follows the actor) or \"dynamic\" (simulated; moves the actor)";
                return fail();
            }
        }
        if (!ReadPositive(source, "mass", out.mass, fieldError) ||
            !ReadNumber(source, "friction", out.friction, fieldError, 0.0f) ||
            !ReadNumber(source, "restitution", out.restitution, fieldError, 0.0f, 1.0f))
            return fail();
        if (out.body == ActorBodyType::Dynamic && !out.collision)
        {
            fieldError = "a dynamic body needs 'collision' (it is simulated through its collider)";
            return fail();
        }
        break;
    }
    }
    return true;
}

ActorResolveResult ResolveActorComponents(const json& classComponents, const json& overrides)
{
    ActorResolveResult result;
    if (!classComponents.is_array())
    {
        result.error = "the actor class has no component list";
        return result;
    }

    std::vector<json> tables;
    std::unordered_set<std::string> names;
    for (const json& table : classComponents)
    {
        ActorComponentDefinition component;
        if (!ParseActorComponent(table, component, result.error))
            return result;
        if (!names.insert(component.name).second)
        {
            result.error = "component '" + component.name + "' is declared twice";
            return result;
        }
        tables.push_back(table);
        result.components.push_back(std::move(component));
    }

    if (overrides.is_object())
    {
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
        {
            const auto found = std::find_if(result.components.begin(), result.components.end(),
                [&](const ActorComponentDefinition& c) { return c.name == it.key(); });
            if (found == result.components.end())
            {
                result.warnings.push_back("the class has no component '" + it.key() + "'; its override is ignored");
                continue;
            }
            if (!it.value().is_object())
            {
                result.warnings.push_back("override of '" + it.key() + "' must be an object; ignored");
                continue;
            }
            const size_t index = static_cast<size_t>(found - result.components.begin());
            json merged = tables[index];
            for (auto field = it.value().begin(); field != it.value().end(); ++field)
            {
                if (field.key() == "name" || field.key() == "type")
                    result.warnings.push_back("override of '" + it.key() + "' cannot change '" +
                                              field.key() + "'; ignored");
                else
                    merged[field.key()] = field.value();
            }
            ActorComponentDefinition component;
            std::string error;
            if (ParseActorComponent(merged, component, error))
                *found = std::move(component);
            else
                result.warnings.push_back("override ignored: " + error);
        }
    }
    else if (!overrides.is_null())
    {
        result.warnings.push_back("component overrides must be an object; ignored");
    }

    for (auto it = result.components.begin(); it != result.components.end();)
    {
        if (it->type == ActorComponentType::ParticleEmitter &&
            !SceneLoader::LoadParticlePreset(it->preset, it->emitter))
        {
            result.warnings.push_back("component '" + it->name + "': particle preset '" + it->preset +
                                      "' could not be loaded; the emitter is skipped");
            it = result.components.erase(it);
            continue;
        }
        ++it;
    }
    return result;
}

} // namespace SE
