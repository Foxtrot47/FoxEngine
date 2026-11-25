#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/MeshCache.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/SceneSaver.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/TransformComponent.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Renderer/Mesh.h"
#include "Engine/Renderer/Texture2D.h"
#include "Engine/Scene/MeshMaterialUtil.h"
#include "Engine/Scene/ActorTypes.h"
#include "Engine/Renderer/PrimitiveMesh.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>

using SE::LevelAssetLoader;
using SE::LevelDiagnosticSeverity;
using json = nlohmann::json;

static void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static bool HasErrorContaining(const SE::LevelAssetLoadResult& result, const char* text)
{
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.severity == LevelDiagnosticSeverity::Error &&
            diagnostic.message.find(text) != std::string::npos)
            return true;
    return false;
}

static void TestValidV2()
{
    const json level = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "Test" },
        { "settings", {
            { "environment", {
                { "skybox", "Assets/Bistro/san_giuseppe_bridge_4k.dds" },
                { "reflectionPanorama", "Assets/Bistro/san_giuseppe_bridge_4k.dds" },
                { "diffuseIBLSaturation", 0.25f }
            } }
        } },
        { "entities", json::array({
            {
                { "id", "0000000000000001" },
                { "name", "Root" },
                { "components", {
                    { "transform", {
                        { "position", { 0.0f, 0.0f, 0.0f } },
                        { "rotationDegrees", { 0.0f, 0.0f, 0.0f } },
                        { "scale", { 1.0f, 2.0f, 3.0f } }
                    } },
                    { "meshRenderer", {
                        { "mesh", "Assets/Bistro/BistroExterior.fbx" }
                    } }
                } }
            },
            {
                { "id", "0000000000000002" },
                { "parent", "0000000000000001" },
                { "name", "Child" },
                { "components", {
                    { "particleEmitter", {
                        { "texture", "Assets/Particles/muzzle_flash.dds" }
                    } }
                } }
            }
        }) }
    };

    const auto result = LevelAssetLoader::Parse(level, "valid");
    Require(result.Succeeded(), "valid v2 level should parse");
    Require(result.asset.entities.size() == 2, "v2 entity count mismatch");
    Require(result.asset.dependencies.size() == 3, "v2 dependency count mismatch");
    Require(!result.asset.convertedFromLegacy, "v2 level marked as legacy");
    Require(result.asset.entities[0].transform.has_value(), "typed transform missing");
    Require(result.asset.entities[0].meshRenderer.has_value(), "typed mesh renderer missing");
    Require(result.asset.entities[0].transform->scale[1] == 2.0f,
            "non-uniform scale was not decoded");

    SE::LevelWorld world;
    Require(world.Instantiate(result.asset), "typed level world failed to instantiate");
    SE::Entity* root = world.FindBySourceID(1);
    SE::Entity* child = world.FindBySourceID(2);
    Require(root && child, "source entity lookup failed");
    Require(root->GetComponent<SE::MeshRendererComponent>(),
            "runtime mesh renderer component missing");
    Require(child->GetComponent<SE::ParticleEmitterComponent>(),
            "runtime particle emitter component missing");
    auto* rootTransform = root->GetComponent<SE::TransformComponent>();
    auto* childTransform = child->GetComponent<SE::TransformComponent>();
    Require(rootTransform && childTransform && childTransform->parent == rootTransform,
            "runtime transform hierarchy was not applied");
    Require(rootTransform->scale.y == 2.0f && rootTransform->scale.z == 3.0f,
            "runtime non-uniform scale mismatch");

    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::BuildDescriptor(result.asset, descriptor),
            "valid v2 descriptor failed to build");
    Require(descriptor.diffuseIBLSaturation == 0.25f,
            "diffuse IBL saturation was not propagated");
}

static void TestTypedFieldValidation()
{
    const json invalid = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "Bad fields" },
        { "settings", { { "environment", { { "diffuseIBLSaturation", 1.5f } } } } },
        { "entities", json::array({
            {
                { "id", "0000000000000001" },
                { "components", {
                    { "meshRenderer", {
                        { "mesh", "Assets/Bistro/BistroExterior.fbx" },
                        { "castShadows", "yes" }
                    } },
                    { "pointLight", { { "radius", -1.0f } } }
                } }
            }
        }) }
    };

    const auto result = LevelAssetLoader::Parse(invalid, "bad-fields");
    Require(!result.Succeeded(), "invalid typed fields unexpectedly succeeded");
    Require(HasErrorContaining(result, "expected a boolean"),
            "typed boolean error was not diagnosed");
    Require(HasErrorContaining(result, "greater than zero"),
            "typed numeric range error was not diagnosed");
    Require(HasErrorContaining(result, "between zero and one"),
            "diffuse IBL saturation range error was not diagnosed");
}

static void TestPlanarReflectionMaterial()
{
    const json level = {
        { "format", "FoxLevel" }, { "version", 2 }, { "name", "Reflection" },
        { "entities", json::array({ {
            { "id", "0000000000000001" },
            { "components", { { "planeRenderer", {
                { "position", { 0.0f, 1.0f, 0.0f } },
                { "material", {
                    { "reflectionMode", "planar" },
                    { "reflectionStrength", 0.75f },
                    { "reflectionResolutionScale", 0.5f },
                    { "roughnessScale", 0.6f },
                    { "metallicScale", 0.9f }
                } }
            } } } }
        } }) }
    };
    const auto result = LevelAssetLoader::Parse(level, "reflection");
    Require(result.Succeeded(), "planar reflection material should parse");
    const auto& material = result.asset.entities[0].primitiveRenderer->material;
    Require(material.planarReflection, "planar reflection mode was not decoded");
    Require(material.reflectionStrength == 0.75f, "reflection strength mismatch");

    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::BuildDescriptor(result.asset, descriptor),
            "reflection descriptor build failed");
    Require(descriptor.objects.size() == 1 && descriptor.objects[0].planarReflection,
            "planar reflection was not propagated to runtime descriptor");
    Require(descriptor.objects[0].roughnessScale == 0.6f &&
            descriptor.objects[0].metallicScale == 0.9f,
            "material scalar controls were not propagated");
}

static void TestAtmosphereSchema()
{
    const json level = {
        { "format", "FoxLevel" }, { "version", 2 }, { "name", "Atmosphere" },
        { "entities", json::array({
            {
                { "id", "0000000000000001" },
                { "components", { { "pointLight", {
                    { "flicker", true }, { "flickerAmplitude", 0.2f },
                    { "flickerFrequency", 6.0f }, { "flickerSeed", 42 }
                } } } }
            },
            {
                { "id", "0000000000000002" },
                { "components", { { "particleEmitter", {
                    { "spawnShape", "box" },
                    { "spawnBoxExtents", { 2.0f, 3.0f, 4.0f } },
                    { "lightingInfluence", 0.75f }
                } } } }
            }
        }) }
    };
    const auto result = LevelAssetLoader::Parse(level, "atmosphere");
    Require(result.Succeeded(), "atmosphere components should parse");
    const auto& light = *result.asset.entities[0].pointLight;
    Require(light.flicker && light.flickerSeed == 42, "light flicker settings mismatch");
    const auto& emitter = *result.asset.entities[1].particleEmitter;
    Require(emitter.spawnShape == 1 && emitter.spawnBoxExtents[2] == 4.0f,
            "box emitter settings mismatch");
    Require(emitter.lightingInfluence == 0.75f, "particle lighting setting mismatch");
}

static void TestValidationFailures()
{
    const json invalid = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "Invalid" },
        { "entities", json::array({
            {
                { "id", "0000000000000001" },
                { "parent", "0000000000000002" },
                { "components", {
                    { "unknown", json::object() }
                } }
            },
            {
                { "id", "0000000000000002" },
                { "parent", "0000000000000001" },
                { "components", json::object() }
            }
        }) }
    };

    const auto result = LevelAssetLoader::Parse(invalid, "invalid");
    Require(!result.Succeeded(), "invalid level unexpectedly succeeded");
    Require(HasErrorContaining(result, "unknown component"), "unknown component not diagnosed");
    Require(HasErrorContaining(result, "parent cycle"), "parent cycle not diagnosed");

    // A legacy "rotation" key in a v2 transform is silently a no-op without this.
    const json legacyKey = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "LegacyKey" },
        { "entities", json::array({
            {
                { "id", "0000000000000001" },
                { "components", { { "transform", { { "rotation", { 0, 90, 0 } } } } } }
            }
        }) }
    };
    const auto legacy = LevelAssetLoader::Parse(legacyKey, "legacyKey");
    bool warned = false;
    for (const auto& d : legacy.diagnostics)
        warned |= d.severity == LevelDiagnosticSeverity::Warning &&
                  d.path.find("transform/rotation") != std::string::npos &&
                  d.message.find("rotationDegrees") != std::string::npos;
    Require(legacy.Succeeded() && warned, "an unknown transform key should warn and name rotationDegrees");

    // Shadow distance is a length in metres, so zero and negatives are invalid.
    const json badShadows = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "BadShadows" },
        { "settings", { { "shadows", { { "distance", -1.0f } } } } },
        { "entities", json::array() }
    };
    const auto shadows = LevelAssetLoader::Parse(badShadows, "badShadows");
    Require(!shadows.Succeeded(), "a negative shadow distance unexpectedly succeeded");
    Require(HasErrorContaining(shadows, "settings.shadows.distance must be a number greater than 0"),
            "shadow distance range error was not diagnosed");
    const json badShutter = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "BadMotionBlurShutter" },
        { "settings", { { "motionBlur", { { "shutterAngle", 0.0f } } } } },
        { "entities", json::array() }
    };
    const auto shutterRes = LevelAssetLoader::Parse(badShutter, "badShutter");
    Require(!shutterRes.Succeeded(), "out-of-range shutterAngle unexpectedly succeeded");
    Require(HasErrorContaining(shutterRes, "settings.motionBlur.shutterAngle out of range"),
            "motionBlur.shutterAngle range error was not diagnosed");

    const json badMaxPx = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "BadMotionBlurMaxPx" },
        { "settings", { { "motionBlur", { { "maxBlurPixels", 256.0f } } } } },
        { "entities", json::array() }
    };
    const auto maxPxRes = LevelAssetLoader::Parse(badMaxPx, "badMaxPx");
    Require(!maxPxRes.Succeeded(), "out-of-range maxBlurPixels unexpectedly succeeded");
    Require(HasErrorContaining(maxPxRes, "settings.motionBlur.maxBlurPixels out of range"),
            "motionBlur.maxBlurPixels range error was not diagnosed");

    const json badSamples = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "BadMotionBlurSamples" },
        { "settings", { { "motionBlur", { { "samples", 64 } } } } },
        { "entities", json::array() }
    };
    const auto samplesRes = LevelAssetLoader::Parse(badSamples, "badSamples");
    Require(!samplesRes.Succeeded(), "out-of-range motionBlur samples unexpectedly succeeded");
    Require(HasErrorContaining(samplesRes, "settings.motionBlur.samples out of range"),
            "motionBlur.samples range error was not diagnosed");
}

static void TestLegacyConversion()
{
    const json legacy = {
        { "name", "Legacy" },
        { "mesh", {
            { "path", "Assets/Bistro/BistroExterior.fbx" },
            { "transform", {
                { "position", { 1.0f, 2.0f, 3.0f } },
                { "rotation", { 90.0f, 0.0f, 0.0f } },
                { "scale", 0.5f }
            } }
        } },
        { "skybox", "Assets/Bistro/san_giuseppe_bridge_4k.dds" },
        { "pointLights", json::array({
            { { "position", { 1.0f, 2.0f, 3.0f } } },
            { { "position", { 4.0f, 5.0f, 6.0f } } }
        }) },
        { "particles", json::array({
            { { "texture", "Assets/Particles/muzzle_flash.dds" } }
        }) }
    };

    const auto result = LevelAssetLoader::Parse(legacy, "legacy");
    Require(result.Succeeded(), "legacy conversion failed");
    Require(result.asset.convertedFromLegacy, "legacy conversion flag missing");
    Require(result.asset.entities.size() == 4, "legacy entity count mismatch");
    Require(result.asset.dependencies.size() == 3, "legacy dependency count mismatch");
}

static void TestRepositoryScenes()
{
    for (const char* path : {
        "Assets/Scenes/bistro.json",
        "Assets/Scenes/showcase.json",
        "Assets/Scenes/sponza.json",
        "Assets/Scenes/suntemple.json"
    })
    {
        const auto result = LevelAssetLoader::LoadFromFile(path);
        Require(result.Succeeded(), "repository scene failed to parse");
        Require(!result.asset.entities.empty(), "repository scene has no entities");

        SE::SceneDescriptor descriptor;
        Require(SE::SceneLoader::LoadFromFile(path, descriptor),
                "repository scene failed to build a runtime descriptor");
    }
}

// Every repository scene must survive descriptor -> SceneSaver -> loader ->
// descriptor with its content intact.
static void TestSaveRoundTrip()
{
    for (const char* path : {
        "Assets/Scenes/bistro.json",
        "Assets/Scenes/showcase.json",
        "Assets/Scenes/sponza.json",
        "Assets/Scenes/suntemple.json"
    })
    {
        SE::SceneDescriptor original;
        Require(SE::SceneLoader::LoadFromFile(path, original),
                "round trip: scene failed to load");

        const std::string text = SE::SceneSaver::ToJson(original);
        const auto reparsed = LevelAssetLoader::Parse(json::parse(text), path);
        Require(reparsed.Succeeded(), "round trip: saved level failed validation");

        SE::SceneDescriptor loaded;
        Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded),
                "round trip: saved level failed to rebuild a descriptor");

        Require(loaded.name == original.name, "round trip: name changed");
        Require(loaded.skybox == original.skybox, "round trip: skybox changed");
        Require(loaded.reflectionPanorama == original.reflectionPanorama,
                "round trip: reflection panorama changed");
        Require(loaded.iblIntensity == original.iblIntensity,
                "round trip: IBL intensity changed");
        Require(loaded.meshes.size() == original.meshes.size(),
                "round trip: mesh count changed");
        for (size_t i = 0; i < original.meshes.size(); ++i)
        {
            Require(loaded.meshes[i].path == original.meshes[i].path,
                    "round trip: mesh path changed");
            Require(loaded.meshes[i].position == original.meshes[i].position &&
                    loaded.meshes[i].rotation == original.meshes[i].rotation &&
                    loaded.meshes[i].scale == original.meshes[i].scale,
                    "round trip: mesh transform changed");
            Require(loaded.meshes[i].alphaCutoff == original.meshes[i].alphaCutoff,
                    "round trip: mesh alpha cutoff changed");
        }
        Require(loaded.pointLights.size() == original.pointLights.size(),
                "round trip: point light count changed");
        for (size_t i = 0; i < original.pointLights.size(); ++i)
        {
            Require(loaded.pointLights[i].position == original.pointLights[i].position &&
                    loaded.pointLights[i].color == original.pointLights[i].color &&
                    loaded.pointLights[i].radius == original.pointLights[i].radius,
                    "round trip: point light changed");
            Require(loaded.pointLights[i].flicker == original.pointLights[i].flicker,
                    "round trip: point light flicker changed");
        }
        Require(loaded.objects.size() == original.objects.size(),
                "round trip: object count changed");
        for (size_t i = 0; i < original.objects.size(); ++i)
        {
            Require(loaded.objects[i].type == original.objects[i].type &&
                    loaded.objects[i].position == original.objects[i].position,
                    "round trip: object changed");
            Require(loaded.objects[i].albedoPath == original.objects[i].albedoPath &&
                    loaded.objects[i].planarReflection == original.objects[i].planarReflection,
                    "round trip: object material changed");
        }
        Require(loaded.particles.size() == original.particles.size(),
                "round trip: emitter count changed");
        for (size_t i = 0; i < original.particles.size(); ++i)
        {
            Require(loaded.particles[i].texture == original.particles[i].texture &&
                    loaded.particles[i].emitRate == original.particles[i].emitRate &&
                    loaded.particles[i].blendMode == original.particles[i].blendMode &&
                    loaded.particles[i].spawnShape == original.particles[i].spawnShape,
                    "round trip: emitter changed");
        }
        Require(loaded.camera.eye == original.camera.eye &&
                loaded.camera.nearZ == original.camera.nearZ &&
                loaded.camera.farZ == original.camera.farZ,
                "round trip: camera changed");
        Require(loaded.sun.elevation == original.sun.elevation &&
                loaded.sun.intensity == original.sun.intensity,
                "round trip: sun changed");
        Require(loaded.physics.gravity == original.physics.gravity &&
                loaded.physics.floor.min == original.physics.floor.min &&
                loaded.physics.floor.max == original.physics.floor.max,
                "round trip: physics changed");
        Require(loaded.toneMapping.op == original.toneMapping.op &&
                loaded.toneMapping.exposure == original.toneMapping.exposure,
                "round trip: tone mapping changed");
        Require(loaded.bloom.enabled == original.bloom.enabled &&
                loaded.bloom.threshold == original.bloom.threshold,
                "round trip: bloom changed");
        Require(loaded.fog.enabled == original.fog.enabled &&
                loaded.fog.density == original.fog.density &&
                loaded.fog.noiseScale == original.fog.noiseScale,
                "round trip: fog changed");

        // A second save of the reloaded descriptor must be byte-identical —
        // the editor's dirty check depends on deterministic serialization.
        Require(SE::SceneSaver::ToJson(loaded) == text,
                "round trip: serialization is not stable");
    }
}

// Shadow distance survives load -> save -> load, and an unset distance stays
// out of the saved settings so existing scenes serialize identically.
static void TestShadowDistanceRoundTrip()
{
    const json level = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "ShadowDistance" },
        { "settings", { { "shadows", { { "distance", 950.0f } } } } },
        { "entities", json::array() }
    };
    const auto parsed = LevelAssetLoader::Parse(level, "shadowDistance");
    Require(parsed.Succeeded(), "shadow distance settings failed to parse");

    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::BuildDescriptor(parsed.asset, descriptor),
            "shadow distance descriptor build failed");
    Require(descriptor.shadows.distance == 950.0f,
            "shadow distance was not propagated to the descriptor");

    const std::string text = SE::SceneSaver::ToJson(descriptor);
    const auto reparsed = LevelAssetLoader::Parse(json::parse(text), "shadowDistance");
    Require(reparsed.Succeeded(), "saved shadow distance failed validation");

    SE::SceneDescriptor loaded;
    Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded),
            "saved shadow distance failed to rebuild a descriptor");
    Require(loaded.shadows.distance == 950.0f,
            "shadow distance did not survive the round trip");
    Require(SE::SceneSaver::ToJson(loaded) == text,
            "shadow distance serialization is not stable");

    SE::SceneDescriptor unset;
    Require(SE::SceneSaver::ToJson(unset).find("\"shadows\"") == std::string::npos,
            "an unset shadow distance must not be serialized");
}
// Motion blur settings survive load -> save -> load, and default disabled
// motion blur is omitted from saved JSON so existing scenes stay clean.
static void TestMotionBlurRoundTrip()
{
    const json level = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "MotionBlur" },
        { "settings", { { "motionBlur", {
            { "enabled", true },
            { "shutterAngle", 240.0f },
            { "maxBlurPixels", 48.0f },
            { "samples", 16 }
        } } } },
        { "entities", json::array() }
    };
    const auto parsed = LevelAssetLoader::Parse(level, "motionBlur");
    Require(parsed.Succeeded(), "motion blur settings failed to parse");

    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::BuildDescriptor(parsed.asset, descriptor),
            "motion blur descriptor build failed");
    Require(descriptor.motionBlur.enabled &&
            descriptor.motionBlur.shutterAngle == 240.0f &&
            descriptor.motionBlur.maxBlurPixels == 48.0f &&
            descriptor.motionBlur.samples == 16,
            "motion blur fields were not propagated to the descriptor");

    const std::string text = SE::SceneSaver::ToJson(descriptor);
    const auto reparsed = LevelAssetLoader::Parse(json::parse(text), "motionBlur");
    Require(reparsed.Succeeded(), "saved motion blur failed validation");

    SE::SceneDescriptor loaded;
    Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded),
            "saved motion blur failed to rebuild a descriptor");
    Require(loaded.motionBlur.enabled &&
            loaded.motionBlur.shutterAngle == 240.0f &&
            loaded.motionBlur.maxBlurPixels == 48.0f &&
            loaded.motionBlur.samples == 16,
            "motion blur did not survive the round trip");
    Require(SE::SceneSaver::ToJson(loaded) == text,
            "motion blur serialization is not stable");

    SE::SceneDescriptor nonDefaultDisabled;
    nonDefaultDisabled.motionBlur.shutterAngle = 90.0f;
    Require(SE::SceneSaver::ToJson(nonDefaultDisabled).find("\"motionBlur\"") != std::string::npos,
            "non-default disabled motion blur must be serialized");

    SE::SceneDescriptor unset;
    Require(SE::SceneSaver::ToJson(unset).find("\"motionBlur\"") == std::string::npos,
            "default disabled motion blur must not be serialized");
}

// settings.reflectionProbes parses, round-trips through the saver, and rejects
// non-positive extents and negative blend distances with the documented errors.
static void TestReflectionProbeSchema()
{
    const json level = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "ReflectionProbes" },
        { "settings", { { "reflectionProbes", json::array({
            { { "position", { 10.0f, 2.0f, -4.0f } }, { "extents", { 20.0f, 6.0f, 8.0f } },
              { "blendDistance", 3.0f } },
            { { "position", { -1.0f, 0.5f, 0.0f } }, { "extents", { 1.0f, 1.0f, 1.0f } } }
        }) } } },
        { "entities", json::array() }
    };
    const auto parsed = LevelAssetLoader::Parse(level, "reflectionProbes");
    Require(parsed.Succeeded(), "valid reflection probes failed to parse");

    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::BuildDescriptor(parsed.asset, descriptor),
            "reflection probe descriptor build failed");
    auto checkProbes = [](const SE::SceneDescriptor& desc, const char* message) {
        Require(desc.reflectionProbes.size() == 2, message);
        const auto& a = desc.reflectionProbes[0];
        const auto& b = desc.reflectionProbes[1];
        Require(a.position == std::array<float, 3>{ 10.0f, 2.0f, -4.0f } &&
                a.extents == std::array<float, 3>{ 20.0f, 6.0f, 8.0f } &&
                a.blendDistance == 3.0f, message);
        Require(b.position == std::array<float, 3>{ -1.0f, 0.5f, 0.0f } &&
                b.extents == std::array<float, 3>{ 1.0f, 1.0f, 1.0f } &&
                b.blendDistance == 5.0f, message);
    };
    checkProbes(descriptor, "reflection probes were not propagated to the descriptor");

    const std::string text = SE::SceneSaver::ToJson(descriptor);
    const auto reparsed = LevelAssetLoader::Parse(json::parse(text), "reflectionProbes");
    Require(reparsed.Succeeded(), "saved reflection probes failed validation");
    SE::SceneDescriptor loaded;
    Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded),
            "saved reflection probes failed to rebuild a descriptor");
    checkProbes(loaded, "reflection probes did not survive the round trip");
    Require(SE::SceneSaver::ToJson(loaded) == text, "reflection probe serialization is not stable");

    SE::SceneDescriptor unset;
    Require(SE::SceneSaver::ToJson(unset).find("\"reflectionProbes\"") == std::string::npos,
            "an empty probe list must not be serialized");

    // The bad probe follows a valid one, so the diagnostics name index 1.
    auto parseProbe = [](const json& probe) {
        const json valid = { { "position", { 0.0f, 0.0f, 0.0f } }, { "extents", { 1.0f, 1.0f, 1.0f } } };
        const json bad = {
            { "format", "FoxLevel" },
            { "version", 2 },
            { "name", "BadReflectionProbe" },
            { "settings", { { "reflectionProbes", json::array({ valid, probe }) } } },
            { "entities", json::array() }
        };
        return LevelAssetLoader::Parse(bad, "badReflectionProbe");
    };
    const auto zeroExtent = parseProbe({ { "position", { 0.0f, 0.0f, 0.0f } },
                                         { "extents", { 4.0f, 0.0f, 4.0f } } });
    Require(!zeroExtent.Succeeded(), "a zero probe extent unexpectedly succeeded");
    Require(HasErrorContaining(zeroExtent,
                "settings.reflectionProbes[1].extents must be an array of 3 numbers greater than 0"),
            "zero probe extent was not diagnosed");

    const auto negativeBlend = parseProbe({ { "position", { 0.0f, 0.0f, 0.0f } },
                                            { "extents", { 4.0f, 4.0f, 4.0f } },
                                            { "blendDistance", -1.0f } });
    Require(!negativeBlend.Succeeded(), "a negative probe blend distance unexpectedly succeeded");
    Require(HasErrorContaining(negativeBlend,
                "settings.reflectionProbes[1].blendDistance must be a number greater than or equal to 0"),
            "negative probe blend distance was not diagnosed");
}

static void TestBistroRuntimeDescriptor()
{
    SE::SceneDescriptor descriptor;
    Require(SE::SceneLoader::LoadFromFile("Assets/Scenes/bistro.json", descriptor),
            "Bistro runtime descriptor failed to load");
    Require(descriptor.meshes.size() == 2, "Bistro should contain exterior and interior meshes");
    Require(descriptor.meshes[0].path == "Assets/Bistro/BistroExterior.fbx",
            "Bistro exterior path mismatch");
    Require(descriptor.meshes[1].path == "Assets/Bistro/BistroInterior.fbx",
            "Bistro interior path mismatch");
    Require(descriptor.diffuseIBLSaturation == 1.0f,
            "Bistro diffuse IBL saturation mismatch");
}

static void TestDependencyPlan()
{
    SE::LevelAsset level;
    SE::LevelEntityDefinition meshA;
    meshA.id = 1;
    meshA.meshRenderer = SE::LevelMeshRendererDefinition{ "Assets\\Models\\..\\Models\\ship.obj" };
    SE::LevelEntityDefinition meshB = meshA;
    meshB.id = 2;
    meshB.meshRenderer->mesh = "Assets/Models/ship.obj";
    SE::LevelEntityDefinition particle;
    particle.id = 3;
    particle.particleEmitter = SE::LevelParticleEmitterDefinition{};
    particle.particleEmitter->texture = "Assets/Particles/soft_circle.png";
    level.entities = { meshA, meshB, particle };
    level.settings["environment"]["skybox"] = "Assets/Environment/sky.dds";

    const auto plan = SE::LevelDependencyPlan::Build(level);
    Require(plan.Find("Assets/Models/ship.obj", SE::LevelDependencyKind::Mesh) != nullptr,
            "normalized mesh dependency missing");
    Require(plan.Find("Assets/Models/ship.obj", SE::LevelDependencyKind::Mesh)->required,
            "geometry dependency should be required");
    size_t meshCount = 0;
    for (const auto& dependency : plan.dependencies)
        if (dependency.kind == SE::LevelDependencyKind::Mesh) ++meshCount;
    Require(meshCount == 1, "mesh dependencies were not deduplicated");
    Require(plan.Find("Assets/Particles/soft_circle.png",
                      SE::LevelDependencyKind::ParticleTexture) != nullptr,
            "particle texture dependency missing");
}

static void TestCpuMeshImport()
{
    SE::MeshSourceData source;
    float lastProgress = 0.0f;
    Require(SE::Mesh::Import("Assets/TestFixtures/tiny.obj", source,
        [&](float progress) {
            Require(progress >= lastProgress, "mesh import progress regressed");
            lastProgress = progress;
            return true;
        }), "tiny OBJ CPU import failed");
    Require(source.subMeshes.size() == 1, "tiny OBJ submesh count mismatch");
    Require(source.subMeshes[0].vertexCount == 3, "tiny OBJ vertex count mismatch");
    Require(source.subMeshes[0].material.albedo.path == "missing_albedo.png",
            "material texture dependency was not discovered");

    SE::MeshSourceData cancelled;
    Require(!SE::Mesh::Import("Assets/TestFixtures/tiny.obj", cancelled,
        [](float) { return false; }), "cancelled mesh import unexpectedly succeeded");
}

static void TestGltfPackedMRDetection()
{
    SE::MeshSourceData source;
    Require(SE::Mesh::Import("Assets/TestFixtures/packed_mr.gltf", source,
                             [](float) { return true; }),
            "packed MR glTF import failed");
    Require(source.subMeshes.size() == 2, "packed MR glTF submesh count mismatch");

    const auto& packed = source.subMeshes[0].material;
    Require(packed.albedo.path == "albedo.png", "glTF base color path mismatch");
    Require(packed.roughness.path == "packed_mr.png",
            "glTF metallic-roughness texture was not routed to the roughness slot");
    Require(packed.workflow == SE::MaterialWorkflow::PackedMR,
            "shared metallic-roughness texture was not flagged as packed");
    Require(packed.normal.path == "normal.png", "glTF normal path mismatch");
    Require(packed.roughnessFactor == 1.0f, "textured material roughness factor should default to 1");

    const auto& scalar = source.subMeshes[1].material;
    Require(scalar.roughness.path.empty(), "texture-less material should have no roughness path");
    Require(scalar.workflow == SE::MaterialWorkflow::SeparateMR,
            "texture-less material must not be flagged as packed");
    Require(scalar.roughnessFactor > 0.34f && scalar.roughnessFactor < 0.36f,
            "scalar roughness factor was not imported");
}

static void TestGltfSpecGlossDetection()
{
    SE::MeshSourceData source;
    Require(SE::Mesh::Import("Assets/TestFixtures/spec_gloss.gltf", source,
                             [](float) { return true; }),
            "spec/gloss glTF import failed");
    Require(source.subMeshes.size() == 3, "spec/gloss glTF submesh count mismatch");

    const auto& textured = source.subMeshes[0].material;
    Require(textured.albedo.path == "diffuse.png", "spec/gloss diffuse path mismatch");
    Require(textured.roughness.path == "spec_gloss.png",
            "specular-glossiness texture was not routed to the roughness slot");
    Require(textured.workflow == SE::MaterialWorkflow::SpecularGloss,
            "spec/gloss material was not detected");
    Require(textured.glossinessFactor > 0.59f && textured.glossinessFactor < 0.61f,
            "glossiness factor was not imported");

    const auto& scalar = source.subMeshes[1].material;
    Require(scalar.workflow == SE::MaterialWorkflow::SeparateMR,
            "texture-less spec/gloss should fall back to the scalar MR path");
    Require(scalar.roughness.path.empty(), "texture-less spec/gloss should have no roughness path");
    Require(scalar.roughnessFactor > 0.74f && scalar.roughnessFactor < 0.76f,
            "texture-less spec/gloss roughness was not derived from glossiness");

    // Node transforms must be baked into vertices (glTF places meshes via nodes),
    // and a mesh referenced by two nodes must yield one submesh per instance.
    float minX = 1e9f, instanceMaxX = -1e9f;
    const auto& placed = source.subMeshes[1];
    for (uint32_t v = placed.firstVertex; v < placed.firstVertex + placed.vertexCount; ++v)
        minX = (std::min)(minX, source.vertices[v].x);
    const auto& instance = source.subMeshes[2];
    for (uint32_t v = instance.firstVertex; v < instance.firstVertex + instance.vertexCount; ++v)
        instanceMaxX = (std::max)(instanceMaxX, source.vertices[v].x);
    Require(minX > 4.0f, "glTF node translation was not baked into mesh vertices");
    Require(instanceMaxX < -3.5f, "instanced glTF mesh was not duplicated per node");
}

static bool SameSlot(const SE::TextureSlot& a, const SE::TextureSlot& b, bool compareTransform)
{
    return a.path == b.path && a.uvSet == b.uvSet &&
           (!compareTransform || (a.scale[0] == b.scale[0] && a.scale[1] == b.scale[1] &&
                                  a.offset[0] == b.offset[0] && a.offset[1] == b.offset[1]));
}

// gltfExtras = false for importer parity: the Assimp path reads standard texture paths and
// UV sets only (texture transforms, occlusion strength and extras.fox are cgltf-only;
// Assimp 6.0.4's glTF reader does not export the occlusion strength).
static bool SameMaterial(const SE::SubMeshInfo& a, const SE::SubMeshInfo& b, bool gltfExtras = true)
{
    const SE::TextureSlot* slotsA[] = { &a.albedo, &a.normal, &a.roughness, &a.emissive, &a.occlusion };
    const SE::TextureSlot* slotsB[] = { &b.albedo, &b.normal, &b.roughness, &b.emissive, &b.occlusion };
    for (size_t i = 0; i < std::size(slotsA); ++i)
        if (!SameSlot(*slotsA[i], *slotsB[i], gltfExtras))
            return false;
    if (gltfExtras &&
        (!SameSlot(a.detailAlbedo, b.detailAlbedo, true) || !SameSlot(a.detailNormal, b.detailNormal, true) ||
         !SameSlot(a.clearcoatTex, b.clearcoatTex, true) ||
         !SameSlot(a.clearcoatRoughnessTex, b.clearcoatRoughnessTex, true) ||
         a.occlusionStrength != b.occlusionStrength || a.occlusionChannel != b.occlusionChannel ||
         a.detailAlbedoBlend != b.detailAlbedoBlend || a.detailAlbedoStrength != b.detailAlbedoStrength ||
         a.detailAlbedoNeutral != b.detailAlbedoNeutral ||
         a.detailNormalStrength != b.detailNormalStrength || a.vertexTint != b.vertexTint ||
         a.clearcoat != b.clearcoat || a.clearcoatRoughness != b.clearcoatRoughness ||
         a.clearcoatMaskFromAlbedoAlpha != b.clearcoatMaskFromAlbedoAlpha))
        return false;
    if (gltfExtras)
    {
        const SE::TerrainMaterial& ta = a.terrain;
        const SE::TerrainMaterial& tb = b.terrain;
        if (a.kind != b.kind || ta.layerCount != tb.layerCount ||
            ta.albedoAlphaIsRoughness != tb.albedoAlphaIsRoughness || ta.overlayStrength != tb.overlayStrength ||
            ta.roughnessScale != tb.roughnessScale || !SameSlot(ta.splat, tb.splat, true) ||
            !SameSlot(ta.overlay, tb.overlay, true))
            return false;
        for (uint32_t i = 0; i < SE::k_maxTerrainLayers; ++i)
            if (ta.layerScale[i] != tb.layerScale[i] || !SameSlot(ta.layerAlbedo[i], tb.layerAlbedo[i], true) ||
                !SameSlot(ta.layerNormal[i], tb.layerNormal[i], true))
                return false;
    }
    return a.alphaMode == b.alphaMode && a.alphaCutoff == b.alphaCutoff &&
           a.workflow == b.workflow && a.roughnessFactor == b.roughnessFactor &&
           a.glossinessFactor == b.glossinessFactor;
}

// Triangles of one submesh as (position, UV0) corners quantized to 1e-4, each rotated
// to start at its smallest corner (winding preserved), sorted; `corners` keeps the
// vertices in the same order for frame comparisons.
struct QuantizedTriangle
{
    std::array<int64_t, 15> key;
    std::array<const SE::MeshVertex*, 3> corners;
    bool operator<(const QuantizedTriangle& other) const { return key < other.key; }
};

static std::vector<QuantizedTriangle> QuantizedTriangles(const SE::MeshSourceData& data,
                                                         const SE::MeshSubMeshSource& sub)
{
    std::vector<QuantizedTriangle> triangles;
    for (uint32_t i = 0; i + 2 < sub.indexCount; i += 3)
    {
        std::array<std::array<int64_t, 5>, 3> keys;
        std::array<const SE::MeshVertex*, 3> vertices;
        for (uint32_t c = 0; c < 3; ++c)
        {
            const SE::MeshVertex& v = data.vertices[sub.firstVertex + data.indices[sub.firstIndex + i + c]];
            vertices[c] = &v;
            const float values[5] = { v.x, v.y, v.z, v.u, v.v };
            for (int k = 0; k < 5; ++k)
                keys[c][k] = static_cast<int64_t>(std::llround(double(values[k]) * 1e4));
        }
        const size_t first = static_cast<size_t>(std::min_element(keys.begin(), keys.end()) - keys.begin());
        QuantizedTriangle triangle;
        for (size_t c = 0; c < 3; ++c)
        {
            std::copy(keys[(first + c) % 3].begin(), keys[(first + c) % 3].end(), triangle.key.begin() + c * 5);
            triangle.corners[c] = vertices[(first + c) % 3];
        }
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

// The cgltf reader must reproduce the Assimp path's submeshes, materials, bounds and
// triangles; vertex counts may differ (Assimp welds identical vertices).
static void TestGltfImporterParity(const std::vector<const char*>& paths)
{
    for (const char* path : paths)
    {
        const std::string context = std::string(" (") + path + ")";
        const auto require = [&context](bool condition, const char* message) {
            if (!condition) throw std::runtime_error(message + context);
        };
        SE::MeshSourceData assimp, gltf;
        std::string error;
        require(SE::Mesh::Import(path, assimp, {}, &error, SE::MeshImporter::Assimp), "Assimp import failed");
        require(SE::Mesh::Import(path, gltf, {}, &error, SE::MeshImporter::Gltf), "cgltf import failed");
        require(gltf.subMeshes.size() == assimp.subMeshes.size(), "importer submesh counts differ");
        require(gltf.sourceFiles == assimp.sourceFiles, "importer dependency lists differ");
        require(gltf.directory == assimp.directory, "importer directories differ");
        const auto within = [](float a, float b) { return std::fabs(a - b) <= 1e-4f; };
        require(within(gltf.bounds.min.x, assimp.bounds.min.x) && within(gltf.bounds.min.y, assimp.bounds.min.y) &&
                within(gltf.bounds.min.z, assimp.bounds.min.z) && within(gltf.bounds.max.x, assimp.bounds.max.x) &&
                within(gltf.bounds.max.y, assimp.bounds.max.y) && within(gltf.bounds.max.z, assimp.bounds.max.z),
                "importer bounds differ");

        // Tangent frames: MikkTSpace and Assimp's CalcTangentSpace differ in detail, so
        // require broad agreement in direction (this catches sign convention errors).
        size_t corners = 0, normalAgree = 0, tangentAgree = 0, bitangentAgree = 0;
        for (size_t s = 0; s < assimp.subMeshes.size(); ++s)
        {
            require(SameMaterial(gltf.subMeshes[s].material, assimp.subMeshes[s].material, false),
                    "importer submesh materials differ");
            const auto a = QuantizedTriangles(assimp, assimp.subMeshes[s]);
            const auto b = QuantizedTriangles(gltf, gltf.subMeshes[s]);
            require(a.size() == b.size(), "importer triangle counts differ");
            for (size_t t = 0; t < a.size(); ++t)
            {
                // One quantization step of slack for values that round to neighbouring steps.
                for (size_t k = 0; k < a[t].key.size(); ++k)
                    require(std::llabs(a[t].key[k] - b[t].key[k]) <= 1, "importer triangles differ");
                for (size_t c = 0; c < 3; ++c)
                {
                    const SE::MeshVertex& r = *a[t].corners[c];
                    const SE::MeshVertex& g = *b[t].corners[c];
                    ++corners;
                    normalAgree += g.nx * r.nx + g.ny * r.ny + g.nz * r.nz > 0.99f;
                    tangentAgree += g.tx * r.tx + g.ty * r.ty + g.tz * r.tz > 0.0f;
                    bitangentAgree += g.bx * r.bx + g.by * r.by + g.bz * r.bz > 0.0f;
                }
            }
        }
        require(normalAgree >= corners * 99 / 100, "importer normals differ");
        require(tangentAgree >= corners * 99 / 100, "importer tangents differ");
        require(bitangentAgree >= corners * 99 / 100, "importer bitangents differ");

        SE::MeshSourceData cancelled;
        std::string cancelError;
        require(!SE::Mesh::Import(path, cancelled, [](float) { return false; }, &cancelError,
                                  SE::MeshImporter::Gltf) && cancelError == "cancelled",
                "cancelled cgltf import did not report cancellation");
    }
}

// Per-submesh bounds: both importers give every submesh an AABB enclosing its own
// vertices, and a MeshCache round trip keeps them.
static void TestSubMeshBounds()
{
    namespace fs = std::filesystem;
    const char* path = "Assets/TestFixtures/spec_gloss.gltf"; // node-instanced primitives
    const auto requireEnclosed = [](const SE::MeshSourceData& data, const char* label) {
        Require(data.subMeshes.size() > 1, (std::string(label) + ": expected several submeshes").c_str());
        for (const SE::MeshSubMeshSource& sub : data.subMeshes)
        {
            Require(sub.bounds.IsValid(), (std::string(label) + ": submesh bounds missing").c_str());
            for (uint32_t v = sub.firstVertex; v < sub.firstVertex + sub.vertexCount; ++v)
            {
                const SE::MeshVertex& vertex = data.vertices[v];
                Require(vertex.x >= sub.bounds.min.x && vertex.x <= sub.bounds.max.x &&
                        vertex.y >= sub.bounds.min.y && vertex.y <= sub.bounds.max.y &&
                        vertex.z >= sub.bounds.min.z && vertex.z <= sub.bounds.max.z,
                        (std::string(label) + ": vertex outside its submesh bounds").c_str());
            }
            Require(sub.bounds.min.x >= data.bounds.min.x && sub.bounds.max.x <= data.bounds.max.x &&
                    sub.bounds.min.y >= data.bounds.min.y && sub.bounds.max.y <= data.bounds.max.y &&
                    sub.bounds.min.z >= data.bounds.min.z && sub.bounds.max.z <= data.bounds.max.z,
                    (std::string(label) + ": submesh bounds exceed the mesh bounds").c_str());
        }
    };

    SE::MeshSourceData assimp, gltf;
    std::string error;
    Require(SE::Mesh::Import(path, assimp, {}, &error, SE::MeshImporter::Assimp), "Assimp import failed");
    Require(SE::Mesh::Import(path, gltf, {}, &error, SE::MeshImporter::Gltf), "cgltf import failed");
    requireEnclosed(assimp, "Assimp");
    requireEnclosed(gltf, "cgltf");

    const fs::path root = fs::temp_directory_path() / "FoxEngineSubMeshBoundsTest";
    fs::remove_all(root);
    const SE::MeshCache cache(root.string());
    SE::MeshSourceData imported, cooked;
    bool hit = true;
    Require(cache.Load(path, imported, {}, nullptr, &hit) && !hit, "first submesh-bounds load did not import");
    Require(cache.Load(path, cooked, {}, nullptr, &hit) && hit, "second submesh-bounds load missed");
    Require(cooked.subMeshes.size() == imported.subMeshes.size(), "cooked submesh count differs");
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
    {
        const SE::AABB& a = cooked.subMeshes[i].bounds;
        const SE::AABB& b = imported.subMeshes[i].bounds;
        Require(a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z &&
                a.max.x == b.max.x && a.max.y == b.max.y && a.max.z == b.max.z,
                "cooked submesh bounds differ from the import");
    }
    requireEnclosed(cooked, "cooked");
    fs::remove_all(root);
}

static void TestMeshCache()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineMeshCacheTest";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    fs::copy_file("Assets/TestFixtures/tiny.obj", root / "src" / "tiny.obj");
    fs::copy_file("Assets/TestFixtures/tiny.mtl", root / "src" / "tiny.mtl");
    const fs::path mtl = root / "src" / "tiny.mtl";
    const std::string source = (root / "src" / "tiny.obj").generic_string();
    const SE::MeshCache cache((root / "cache").string());
    const std::string entry = cache.EntryPath(source);

    SE::MeshSourceData imported;
    bool hit = true;
    Require(cache.Load(source, imported, {}, nullptr, &hit) && !hit,
            "first mesh cache load did not import");
    Require(fs::exists(entry), "mesh cache entry was not written");
    Require(imported.sourceFiles.size() == 2, "OBJ and MTL were not both recorded as dependencies");

    SE::MeshSourceData cooked;
    Require(cache.Load(source, cooked, {}, nullptr, &hit) && hit, "second mesh cache load missed");
    Require(cooked.vertices.size() == imported.vertices.size() &&
            std::memcmp(cooked.vertices.data(), imported.vertices.data(),
                        imported.vertices.size() * sizeof(SE::MeshVertex)) == 0,
            "cooked vertices differ from the import");
    Require(cooked.indices == imported.indices, "cooked indices differ from the import");
    Require(cooked.subMeshes.size() == imported.subMeshes.size(), "cooked submesh count differs");
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
    {
        const auto& a = cooked.subMeshes[i];
        const auto& b = imported.subMeshes[i];
        Require(a.firstVertex == b.firstVertex && a.vertexCount == b.vertexCount &&
                a.firstIndex == b.firstIndex && a.indexCount == b.indexCount,
                "cooked submesh ranges differ from the import");
        Require(SameMaterial(a.material, b.material), "cooked submesh material differs from the import");
    }
    Require(cooked.directory == imported.directory, "cooked texture directory differs from the import");
    Require(cooked.sourceFiles == imported.sourceFiles, "cooked dependency list differs from the import");
    Require(cooked.bounds.min.x == imported.bounds.min.x && cooked.bounds.min.y == imported.bounds.min.y &&
            cooked.bounds.min.z == imported.bounds.min.z && cooked.bounds.max.x == imported.bounds.max.x &&
            cooked.bounds.max.y == imported.bounds.max.y && cooked.bounds.max.z == imported.bounds.max.z,
            "cooked bounds differ from the import");

    Require(cache.Cook(source, false) == SE::MeshCookResult::UpToDate, "current entry was not up to date");
    Require(cache.Cook(source, true) == SE::MeshCookResult::Cooked, "forced cook did not re-cook");

    {
        std::ofstream edit(mtl, std::ios::app);
        edit << "\n# edited\n";
    }
    fs::last_write_time(mtl, fs::last_write_time(mtl) + std::chrono::seconds(2));
    Require(cache.Load(source, cooked, {}, nullptr, &hit) && !hit,
            "editing a side file did not invalidate the cooked entry");

    fs::resize_file(entry, 100);
    Require(cache.Load(source, cooked, {}, nullptr, &hit) && !hit, "truncated entry was not re-imported");
    Require(cache.Load(source, cooked, {}, nullptr, &hit) && hit, "re-imported entry was not stored");

    Require(!cache.Load(source, cooked, [](float) { return false; }),
            "cancelled cached load unexpectedly succeeded");

    fs::remove(root / "src" / "tiny.obj");
    fs::remove(mtl);
    Require(cache.Load(source, cooked, {}, nullptr, &hit) && hit,
            "cooked entry was not used for a missing source");
    Require(cooked.subMeshes.size() == 1 && cooked.subMeshes[0].material.albedo.path == "missing_albedo.png",
            "cooked-only load lost material data");
    Require(cache.Cook(source, false) == SE::MeshCookResult::Failed, "cooking a missing source succeeded");

    SE::AssetManager assets;
    assets.CacheMesh(source, std::make_shared<SE::Mesh>());
    Require(assets.FindMesh(source) != nullptr, "mesh with a missing source was evicted from the asset cache");

    fs::remove_all(root);
}

static void TestVertexStreams()
{
    const auto checkStreamsQuad = [](const SE::MeshSourceData& data, const char* label) {
        const auto check = [label](bool ok, const char* msg) {
            if (!ok)
                throw std::runtime_error(std::string(label) + ": " + msg);
        };
        check(data.subMeshes.size() == 1 && data.vertices.size() == 4 && data.indices.size() == 6,
              "unexpected quad topology");
        const float expectedColors[4][4] = {
            { 1.0f, 0.0f, 0.0f, 1.0f }, // (0, 0) red
            { 0.0f, 1.0f, 0.0f, 1.0f }, // (1, 0) green
            { 0.0f, 0.0f, 1.0f, 1.0f }, // (1, 1) blue
            { 1.0f, 1.0f, 1.0f, 1.0f }, // (0, 1) white
        };
        bool seenCorner[4] = {};
        for (const SE::MeshVertex& v : data.vertices)
        {
            check(std::fabs(v.u1 - 2.0f * v.u) < 1e-5f && std::fabs(v.v1 - 2.0f * v.v) < 1e-5f,
                  "UV1 does not match 2*UV0");
            check(v.u2 == v.u && v.v2 == v.v,
                  "missing UV2 did not default to UV0");
            const int ix = v.x > 0.5f ? 1 : 0;
            const int iy = v.y > 0.5f ? 1 : 0;
            const int corner = iy == 0 ? (ix == 0 ? 0 : 1) : (ix == 1 ? 2 : 3);
            seenCorner[corner] = true;
            const float rgba[4] = {
                static_cast<float>(v.color & 0xFFu) / 255.0f,
                static_cast<float>((v.color >> 8) & 0xFFu) / 255.0f,
                static_cast<float>((v.color >> 16) & 0xFFu) / 255.0f,
                static_cast<float>((v.color >> 24) & 0xFFu) / 255.0f,
            };
            for (int c = 0; c < 4; ++c)
                check(std::fabs(rgba[c] - expectedColors[corner][c]) <= 1.0f / 255.0f,
                      "vertex color channel mismatch");
        }
        check(seenCorner[0] && seenCorner[1] && seenCorner[2] && seenCorner[3],
              "missing quad corner");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/streams.gltf", imported),
            "failed to import streams.gltf");
    checkStreamsQuad(imported, "streams.gltf (Auto/cgltf)");

    SE::MeshSourceData assimpQuad;
    Require(SE::Mesh::Import("Assets/TestFixtures/streams.gltf", assimpQuad, {}, nullptr,
                             SE::MeshImporter::Assimp),
            "failed to import streams.gltf via Assimp");
    checkStreamsQuad(assimpQuad, "streams.gltf (Assimp)");

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineVertexStreamsCacheTest";
    fs::remove_all(root);
    const SE::MeshCache cache((root / "cache").string());
    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load("Assets/TestFixtures/streams.gltf", firstLoad, {}, nullptr, &hit) && !hit,
            "streams.gltf first cache load did not import");
    Require(cache.Load("Assets/TestFixtures/streams.gltf", cachedLoad, {}, nullptr, &hit) && hit,
            "streams.gltf second cache load missed");
    Require(cachedLoad.vertices.size() == firstLoad.vertices.size() &&
            std::memcmp(cachedLoad.vertices.data(), firstLoad.vertices.data(),
                        firstLoad.vertices.size() * sizeof(SE::MeshVertex)) == 0,
            "streams.gltf MeshCache round trip altered vertex streams");
    fs::remove_all(root);

    SE::MeshSourceData obj;
    Require(SE::Mesh::Import("Assets/TestFixtures/tiny.obj", obj),
            "failed to import tiny.obj");
    Require(!obj.vertices.empty(), "tiny.obj produced no vertices");
    for (const SE::MeshVertex& v : obj.vertices)
    {
        Require(v.u1 == v.u && v.v1 == v.v && v.u2 == v.u && v.v2 == v.v,
                "tiny.obj missing UV1/UV2 did not default to UV0");
        Require(v.color == 0xFFFFFFFFu,
                "tiny.obj missing vertex color did not default to white");
    }
}

// Per-slot UV sets and tiling (texCoord, KHR_texture_transform), occlusion and the
// extras.fox detail slots, through import and a MeshCache round trip.
static void TestTextureSlots()
{
    const auto checkSlot = [](const SE::TextureSlot& slot, const char* path, uint8_t uvSet,
                              float sx, float sy, float ox, float oy, const char* label) {
        const auto same = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
        if (slot.path != path || slot.uvSet != uvSet || !same(slot.scale[0], sx) || !same(slot.scale[1], sy) ||
            !same(slot.offset[0], ox) || !same(slot.offset[1], oy))
            throw std::runtime_error(std::string("slots.gltf: ") + label + " slot mismatch");
    };
    const auto checkSource = [&](const SE::MeshSourceData& data) {
        Require(data.subMeshes.size() == 5, "slots.gltf submesh count mismatch");
        const SE::SubMeshInfo& slots = data.subMeshes[0].material;
        checkSlot(slots.albedo, "albedo.png", 0, 4.0f, 4.0f, 0.5f, 0.0f, "albedo");
        checkSlot(slots.normal, "normal.png", 0, 2.0f, 2.0f, 0.0f, 0.0f, "normal");
        checkSlot(slots.roughness, "mr.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "roughness");
        checkSlot(slots.emissive, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "emissive");
        checkSlot(slots.occlusion, "ao.png", 1, 1.0f, 1.0f, 0.0f, 0.0f, "occlusion");
        checkSlot(slots.detailAlbedo, "detail.png", 1, 8.0f, 8.0f, 0.25f, 0.5f, "detail albedo");
        checkSlot(slots.detailNormal, "detail_normal.dds", 2, 16.0f, 16.0f, 0.0f, 0.0f, "detail normal");
        Require(slots.occlusionStrength == 0.5f, "slots.gltf occlusion strength mismatch");
        Require(slots.occlusionChannel == 1, "slots.gltf occlusion channel mismatch");
        Require(slots.detailAlbedoBlend == SE::DetailBlend::Overlay &&
                slots.detailAlbedoStrength == 1.0f,
                "slots.gltf absent detailAlbedo mode/strength did not default to Overlay/1.0");
        Require(slots.detailNormalStrength == 0.75f, "slots.gltf detail normal strength mismatch");
        Require(slots.workflow == SE::MaterialWorkflow::PackedMR, "slots.gltf workflow mismatch");
        size_t paths = 0;
        SE::ForEachTexturePath(slots, [&paths](const std::string&) { ++paths; });
        Require(paths == 6, "ForEachTexturePath did not enumerate every slot path");

        const SE::SubMeshInfo& defaults = data.subMeshes[1].material;
        checkSlot(defaults.occlusion, "ao.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default occlusion");
        checkSlot(defaults.detailAlbedo, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default detail albedo");
        checkSlot(defaults.detailNormal, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default detail normal");
        Require(defaults.occlusionStrength == 1.0f && defaults.occlusionChannel == 0 &&
                defaults.detailAlbedoBlend == SE::DetailBlend::Overlay &&
                defaults.detailAlbedoStrength == 1.0f &&
                defaults.detailNormalStrength == 1.0f, "slots.gltf default scalars mismatch");

        const SE::SubMeshInfo& mul = data.subMeshes[2].material;
        checkSlot(mul.detailAlbedo, "detail.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "detail multiply");
        Require(mul.detailAlbedoBlend == SE::DetailBlend::Multiply &&
                mul.detailAlbedoStrength == 0.5f,
                "slots.gltf detailAlbedo multiply mode/strength mismatch");

        const SE::SubMeshInfo& blend = data.subMeshes[3].material;
        checkSlot(blend.detailAlbedo, "detail.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "detail alphaBlend");
        Require(blend.detailAlbedoBlend == SE::DetailBlend::AlphaBlend &&
                blend.detailAlbedoStrength == 1.0f,
                "slots.gltf detailAlbedo alphaBlend mode/strength mismatch");

        const SE::SubMeshInfo& unknown = data.subMeshes[4].material;
        checkSlot(unknown.detailAlbedo, "detail.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "detail unknown");
        Require(unknown.detailAlbedoBlend == SE::DetailBlend::Overlay &&
                unknown.detailAlbedoStrength == 1.0f,
                "slots.gltf unknown detailAlbedo mode/non-number strength did not fall back to Overlay/1.0");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/slots.gltf", imported), "failed to import slots.gltf");
    checkSource(imported);

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineTextureSlotsCacheTest";
    fs::remove_all(root);
    const SE::MeshCache cache((root / "cache").string());
    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load("Assets/TestFixtures/slots.gltf", firstLoad, {}, nullptr, &hit) && !hit,
            "slots.gltf first cache load did not import");
    Require(cache.Load("Assets/TestFixtures/slots.gltf", cachedLoad, {}, nullptr, &hit) && hit,
            "slots.gltf second cache load missed");
    checkSource(cachedLoad);
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
        Require(SameMaterial(cachedLoad.subMeshes[i].material, imported.subMeshes[i].material),
                "slots.gltf MeshCache round trip altered material slots");
    fs::remove_all(root);
}
// extras.fox.vertexTint parsing ("multiply", "albedoAlphaMask", unknown/absent -> None)
// and MeshCache round trip.
static void TestVertexTint()
{
    const auto checkSource = [](const SE::MeshSourceData& data) {
        Require(data.subMeshes.size() == 5, "tint.gltf submesh count mismatch");
        Require(data.subMeshes[0].material.vertexTint == SE::VertexTint::Multiply,
                "tint.gltf multiply mode mismatch");
        Require(data.subMeshes[1].material.vertexTint == SE::VertexTint::AlbedoAlphaMask,
                "tint.gltf albedoAlphaMask mode mismatch");
        Require(data.subMeshes[2].material.vertexTint == SE::VertexTint::None,
                "tint.gltf unknown string should default to None");
        Require(data.subMeshes[3].material.vertexTint == SE::VertexTint::None,
                "tint.gltf non-string vertexTint should default to None");
        Require(data.subMeshes[4].material.vertexTint == SE::VertexTint::None,
                "tint.gltf absent vertexTint should default to None");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/tint.gltf", imported), "failed to import tint.gltf");
    checkSource(imported);

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineVertexTintCacheTest";
    fs::remove_all(root);
    const SE::MeshCache cache((root / "cache").string());
    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load("Assets/TestFixtures/tint.gltf", firstLoad, {}, nullptr, &hit) && !hit,
            "tint.gltf first cache load did not import");
    Require(cache.Load("Assets/TestFixtures/tint.gltf", cachedLoad, {}, nullptr, &hit) && hit,
            "tint.gltf second cache load missed");
    checkSource(cachedLoad);
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
        Require(SameMaterial(cachedLoad.subMeshes[i].material, imported.subMeshes[i].material),
                "tint.gltf MeshCache round trip altered vertexTint");
    fs::remove_all(root);
}

// extras.fox.material .foxmat terrain sidecars: parsed fields (texture paths rebased from the
// sidecar to the mesh directory), the sidecar as a cache dependency, and Standard fallbacks for
// missing, malformed and invalid sidecars.
static void TestTerrainMaterial()
{
    const auto checkTerrain = [](const SE::MeshSourceData& data, const std::string& foxmat) {
        Require(data.subMeshes.size() == 2, "terrain.gltf submesh count mismatch");
        const SE::SubMeshInfo& info = data.subMeshes[0].material;
        Require(info.kind == SE::MaterialKind::Terrain, "terrain.gltf material is not a terrain material");
        const SE::TerrainMaterial& terrain = info.terrain;
        Require(terrain.layerCount == 4, "terrain layer count mismatch");
        const char* albedo[] = { "terrain/layers/grass_a_albr.dds", "terrain/layers/grass_b_albr.dds",
                                 "terrain/layers/dirt_c_albr.dds", "terrain/layers/dirt_d_albr.dds" };
        const char* normal[] = { "terrain/layers/grass_na_nm.dds", "terrain/layers/grass_na_nm.dds",
                                 "terrain/layers/dirt_nb_nm.dds", "" };
        const float scale[] = { 0.195f, 0.25f, 0.5f, 1.0f };
        for (uint32_t i = 0; i < SE::k_maxTerrainLayers; ++i)
        {
            Require(terrain.layerAlbedo[i].path == albedo[i], "terrain layer albedo path mismatch");
            Require(terrain.layerNormal[i].path == normal[i], "terrain layer normal path mismatch");
            Require(terrain.layerScale[i] == scale[i], "terrain layer world scale mismatch");
        }
        Require(terrain.albedoAlphaIsRoughness, "terrain albedoAlphaIsRoughness mismatch");
        Require(terrain.splat.path == "ra_splat.dds" && terrain.splat.uvSet == 1 &&
                terrain.splat.scale[0] == 1.0f && terrain.splat.scale[1] == 1.0f, "terrain splat mismatch");
        Require(terrain.overlay.path == "ra_overlay.dds" && terrain.overlay.uvSet == 2 &&
                terrain.overlay.scale[0] == 2.0f && terrain.overlay.scale[1] == 2.5f &&
                terrain.overlayStrength == 0.91f, "terrain overlay mismatch");
        Require(terrain.roughnessScale == 0.8f, "terrain roughness scale mismatch");
        Require(info.albedo.path == "fallback_albedo.png", "terrain material lost its glTF albedo slot");
        size_t paths = 0;
        SE::ForEachTexturePath(info, [&paths](const std::string&) { ++paths; });
        Require(paths == 10, "ForEachTexturePath did not enumerate every terrain texture");
        Require(std::find(data.sourceFiles.begin(), data.sourceFiles.end(), foxmat) != data.sourceFiles.end(),
                "the .foxmat sidecar is not a mesh dependency");

        const SE::SubMeshInfo& missing = data.subMeshes[1].material;
        Require(missing.kind == SE::MaterialKind::Standard && missing.albedo.path == "fallback_albedo.png",
                "a missing .foxmat did not fall back to the glTF material");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/terrain.gltf", imported), "failed to import terrain.gltf");
    checkTerrain(imported, "Assets/TestFixtures/terrain/terrain.foxmat");

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineTerrainMaterialTest";
    fs::remove_all(root);
    fs::create_directories(root / "src" / "terrain");
    fs::copy_file("Assets/TestFixtures/terrain.gltf", root / "src" / "terrain.gltf");
    const fs::path foxmat = root / "src" / "terrain" / "terrain.foxmat";
    fs::copy_file("Assets/TestFixtures/terrain/terrain.foxmat", foxmat);
    const std::string source = (root / "src" / "terrain.gltf").generic_string();
    const SE::MeshCache cache((root / "cache").string());

    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load(source, firstLoad, {}, nullptr, &hit) && !hit, "terrain.gltf first cache load did not import");
    Require(cache.Load(source, cachedLoad, {}, nullptr, &hit) && hit, "terrain.gltf second cache load missed");
    checkTerrain(cachedLoad, foxmat.lexically_normal().generic_string());
    for (size_t i = 0; i < firstLoad.subMeshes.size(); ++i)
        Require(SameMaterial(cachedLoad.subMeshes[i].material, firstLoad.subMeshes[i].material),
                "terrain.gltf MeshCache round trip altered the terrain material");

    const auto rewrite = [&foxmat](const std::string& text) {
        const auto written = fs::last_write_time(foxmat);
        std::ofstream(foxmat, std::ios::binary | std::ios::trunc) << text;
        fs::last_write_time(foxmat, written + std::chrono::seconds(2));
    };
    const auto loadStandard = [&](const std::string& label) {
        SE::MeshSourceData reloaded;
        Require(cache.Load(source, reloaded, {}, nullptr, &hit) && !hit,
                ("editing the .foxmat did not invalidate the cooked entry (" + label + ")").c_str());
        const SE::SubMeshInfo& info = reloaded.subMeshes[0].material;
        Require(info.kind == SE::MaterialKind::Standard && info.albedo.path == "fallback_albedo.png",
                (label + " .foxmat did not fall back to the glTF material").c_str());
    };
    rewrite("{ \"format\": \"FoxMaterial\", \"version\": 1, \"type\": \"terrain\", ");
    loadStandard("malformed");
    rewrite("{ \"format\": \"FoxMaterial\", \"version\": 1, \"type\": \"terrain\","
            " \"layers\": [ { \"albedo\": \"a.dds\", \"worldScale\": 0 } ] }");
    loadStandard("invalid");
    rewrite("{ \"format\": \"FoxMaterial\", \"version\": 1, \"type\": \"terrain\","
            " \"layers\": [ { \"albedo\": \"a.dds\", \"worldScale\": 1 } ],"
            " \"overlay\": { \"texture\": \"o.dds\", \"uvScale\": [2, 0] } }");
    loadStandard("invalid uvScale");
    fs::remove_all(root);
}

// KHR_materials_clearcoat factors/textures, extras.fox.clearcoatMask ("albedoAlpha"
// honoured only for Opaque), ForEachTexturePath enumeration, and MeshCache round trip.
static void TestClearcoat()
{
    const auto checkSlot = [](const SE::TextureSlot& slot, const char* path, uint8_t uvSet,
                              float sx, float sy, float ox, float oy, const char* label) {
        Require(slot.path == path && slot.uvSet == uvSet &&
                slot.scale[0] == sx && slot.scale[1] == sy &&
                slot.offset[0] == ox && slot.offset[1] == oy,
                (std::string("clearcoat.gltf ") + label + " slot mismatch").c_str());
    };
    const auto checkSource = [&](const SE::MeshSourceData& data) {
        Require(data.subMeshes.size() == 5, "clearcoat.gltf submesh count mismatch");

        const SE::SubMeshInfo& full = data.subMeshes[0].material;
        Require(full.clearcoat == 0.85f && full.clearcoatRoughness == 0.15f,
                "clearcoat.gltf full clearcoat factors mismatch");
        Require(full.clearcoatMaskFromAlbedoAlpha,
                "clearcoat.gltf opaque albedoAlpha mask was not set");
        checkSlot(full.clearcoatTex, "coat.png", 1, 2.0f, 2.0f, 0.25f, 0.5f, "clearcoat");
        checkSlot(full.clearcoatRoughnessTex, "coat_rough.dds", 2, 4.0f, 4.0f, 0.0f, 0.0f, "clearcoat roughness");
        size_t paths = 0;
        SE::ForEachTexturePath(full, [&paths](const std::string&) { ++paths; });
        Require(paths == 2, "ForEachTexturePath did not enumerate clearcoat texture slots");

        const SE::SubMeshInfo& cutout = data.subMeshes[1].material;
        Require(cutout.alphaMode == SE::AlphaMode::Cutout &&
                cutout.clearcoat == 1.0f && cutout.clearcoatRoughness == 0.05f &&
                !cutout.clearcoatMaskFromAlbedoAlpha,
                "clearcoat.gltf cutout material must ignore albedoAlpha clearcoat mask");

        const SE::SubMeshInfo& blend = data.subMeshes[2].material;
        Require(blend.alphaMode == SE::AlphaMode::Transparent &&
                blend.clearcoat == 0.5f && blend.clearcoatRoughness == 0.2f &&
                !blend.clearcoatMaskFromAlbedoAlpha,
                "clearcoat.gltf blend material must ignore albedoAlpha clearcoat mask");

        const SE::SubMeshInfo& unknown = data.subMeshes[3].material;
        Require(unknown.clearcoat == 1.0f && unknown.clearcoatRoughness == 0.0f &&
                !unknown.clearcoatMaskFromAlbedoAlpha,
                "clearcoat.gltf unknown clearcoatMask did not default to false");

        const SE::SubMeshInfo& defaults = data.subMeshes[4].material;
        Require(defaults.clearcoat == 0.0f && defaults.clearcoatRoughness == 0.0f &&
                !defaults.clearcoatMaskFromAlbedoAlpha,
                "clearcoat.gltf default clearcoat scalars mismatch");
        checkSlot(defaults.clearcoatTex, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default clearcoat");
        checkSlot(defaults.clearcoatRoughnessTex, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default clearcoat roughness");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/clearcoat.gltf", imported), "failed to import clearcoat.gltf");
    checkSource(imported);

    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "FoxEngineClearcoatCache";
    std::filesystem::remove_all(cacheDir);
    const SE::MeshCache cache(cacheDir.string());
    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load("Assets/TestFixtures/clearcoat.gltf", firstLoad, {}, nullptr, &hit) && !hit,
            "clearcoat.gltf first cache load did not import");
    Require(cache.Load("Assets/TestFixtures/clearcoat.gltf", cachedLoad, {}, nullptr, &hit) && hit,
            "clearcoat.gltf second cache load missed");
    checkSource(cachedLoad);
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
        Require(SameMaterial(cachedLoad.subMeshes[i].material, imported.subMeshes[i].material),
                "clearcoat.gltf MeshCache round trip altered clearcoat material");
    std::filesystem::remove_all(cacheDir);
}

// World-planar slots (extras.fox.worldUV -> uvSet 3, zero offset) and detail albedo
// overlay neutral (extras.fox.detailAlbedo.neutral), through import and MeshCache round trip.
static void TestWorldUVAndDetailNeutral()
{
    const auto same = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
    const auto checkSlot = [&](const SE::TextureSlot& slot, const char* path, uint8_t uvSet,
                               float sx, float sy, float ox, float oy, const char* label) {
        if (slot.path != path || slot.uvSet != uvSet || !same(slot.scale[0], sx) || !same(slot.scale[1], sy) ||
            !same(slot.offset[0], ox) || !same(slot.offset[1], oy))
            throw std::runtime_error(std::string("world_uv.gltf: ") + label + " slot mismatch");
    };
    const auto checkSource = [&](const SE::MeshSourceData& data) {
        Require(data.subMeshes.size() == 3, "world_uv.gltf submesh count mismatch");

        const SE::SubMeshInfo& world = data.subMeshes[0].material;
        checkSlot(world.albedo, "albedo.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "albedo");
        checkSlot(world.normal, "normal.png", 3, 0.2f, 0.2f, 0.0f, 0.0f, "world normal");
        checkSlot(world.detailAlbedo, "detail.png", 3, 0.25f, 0.5f, 0.0f, 0.0f, "world detail albedo");
        checkSlot(world.roughness, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "roughness");
        checkSlot(world.emissive, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "emissive");
        checkSlot(world.occlusion, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "occlusion");
        checkSlot(world.detailNormal, "", 0, 1.0f, 1.0f, 0.0f, 0.0f, "detail normal");
        Require(same(world.detailAlbedoNeutral, 0.47f), "world_uv.gltf detailAlbedoNeutral mismatch");

        const SE::SubMeshInfo& defaults = data.subMeshes[1].material;
        checkSlot(defaults.albedo, "albedo.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default albedo");
        checkSlot(defaults.detailAlbedo, "detail.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "default detail albedo");
        Require(same(defaults.detailAlbedoNeutral, 0.2176376f),
                "world_uv.gltf absent detailAlbedo.neutral did not default to 0.2176376");

        const SE::SubMeshInfo& invalid = data.subMeshes[2].material;
        checkSlot(invalid.albedo, "albedo.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "invalid worldUV albedo");
        checkSlot(invalid.normal, "normal.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "invalid worldUV normal");
        checkSlot(invalid.detailAlbedo, "detail.png", 0, 1.0f, 1.0f, 0.0f, 0.0f, "invalid neutral detail");
        Require(same(invalid.detailAlbedoNeutral, 0.2176376f),
                "world_uv.gltf invalid detailAlbedo.neutral did not fall back to default");
    };

    SE::MeshSourceData imported;
    Require(SE::Mesh::Import("Assets/TestFixtures/world_uv.gltf", imported), "failed to import world_uv.gltf");
    checkSource(imported);

    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "FoxEngineWorldUVCache";
    std::filesystem::remove_all(cacheDir);
    const SE::MeshCache cache(cacheDir.string());
    SE::MeshSourceData firstLoad, cachedLoad;
    bool hit = true;
    Require(cache.Load("Assets/TestFixtures/world_uv.gltf", firstLoad, {}, nullptr, &hit) && !hit,
            "world_uv.gltf first cache load did not import");
    Require(cache.Load("Assets/TestFixtures/world_uv.gltf", cachedLoad, {}, nullptr, &hit) && hit,
            "world_uv.gltf second cache load missed");
    checkSource(cachedLoad);
    for (size_t i = 0; i < imported.subMeshes.size(); ++i)
        Require(SameMaterial(cachedLoad.subMeshes[i].material, imported.subMeshes[i].material),
                "world_uv.gltf MeshCache round trip altered material");
    std::filesystem::remove_all(cacheDir);
}

// glTF image URIs are relative to the mesh file; the converted DDS sits beside
// the authored image, and the Textures/ convention still wins when present.
static void TestMaterialTexturePath()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "FoxEngineTexturePathTest";
    fs::remove_all(root);
    fs::create_directories(root / "textures_png");
    std::ofstream(root / "textures_png" / "a.dds").put('x');
    std::ofstream(root / "textures_png" / "b.png").put('x');
    const std::string dir = root.generic_string() + "/";

    Require(SE::MaterialTexturePath(dir, "textures_png/a.png") == dir + "textures_png/a.dds",
            "relative glTF URI did not resolve to its sibling DDS");
    Require(SE::MaterialTexturePath(dir, "textures_png\\b.png") == dir + "textures_png/b.png",
            "relative source image was not used when no DDS exists");
    Require(SE::MaterialTexturePath(dir, "textures_png/missing.png") == dir + "missing.dds",
            "missing texture did not fall back to <dir>/<stem>.dds");

    fs::create_directories(root / "Textures");
    std::ofstream(root / "Textures" / "a.dds").put('x');
    Require(SE::MaterialTexturePath(dir, "textures_png/a.png") == dir + "Textures/a.dds",
            "Textures/<stem>.dds no longer takes precedence");

    fs::remove_all(root);
}

static void TestCpuTextureDecode()
{
    SE::TextureSourceData png;
    Require(SE::Texture2D::Decode(L"Assets/Particles/soft_circle.png", png),
            "PNG CPU decode failed");
    Require(png.width > 0 && png.height > 0 && !png.subresources.empty(),
            "PNG decode returned no image data");
    SE::TextureSourceData dds;
    Require(SE::Texture2D::Decode(L"Assets/Particles/soft_circle.dds", dds),
            "DDS CPU decode failed");
    Require(dds.width > 0 && dds.height > 0 && !dds.subresources.empty(),
            "DDS decode returned no image data");
}

static SE::LevelLoadStatus WaitForCpuPreparation(SE::LevelLoadOperation& operation)
{
    float progress = 0.0f;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto status = operation.GetStatus();
        Require(status.progress >= progress, "level load progress regressed");
        progress = status.progress;
        if (status.state != SE::LevelLoadState::Preparing) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("level CPU preparation timed out");
}

static void TestOperationFailuresAndCancellation()
{
    {
        SE::LevelLoadOperation operation;
        operation.Start("async_level.json", {});
        const auto status = WaitForCpuPreparation(operation);
        Require(status.state == SE::LevelLoadState::Uploading,
                "optional failures should not reject level preparation");
        Require(!status.warnings.empty(), "optional dependency warning missing");
        Require(status.dependencyCount == 2,
                "dynamic material dependency was not added to the operation");
        operation.Cancel();
        Require(operation.GetStatus().state == SE::LevelLoadState::Cancelled,
                "operation cancellation state mismatch");
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("missing_geometry.json", {});
        const auto status = WaitForCpuPreparation(operation);
        Require(status.state == SE::LevelLoadState::Failed,
                "required geometry failure did not reject preparation");
        Require(status.requiredFailures == 1, "required failure count mismatch");
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("async_level.json", {});
        operation.Cancel();
        Require(operation.GetStatus().state == SE::LevelLoadState::Cancelled,
                "active operation did not cancel cleanly");
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("async_level.json", {});
    }
}

static void TestCachedMeshSkipsImport()
{
    SE::AssetCacheSnapshot cache;
    cache.meshes.emplace("Assets/TestFixtures/not_here.obj", std::make_shared<SE::Mesh>());
    SE::LevelLoadOperation operation;
    operation.Start("missing_geometry.json", std::move(cache));
    const auto status = WaitForCpuPreparation(operation);
    Require(status.state == SE::LevelLoadState::Uploading,
            "cached required mesh still entered the import path");
    Require(status.cacheHits == 1, "cached mesh hit was not reported");
    Require(status.requiredFailures == 0, "cached mesh was treated as a failed import");
    operation.Cancel();
}

static void TestAssetCacheRetainsReleasedHandles()
{
    SE::AssetManager assets;
    auto mesh = std::make_shared<SE::Mesh>();
    assets.CacheMesh("Assets/TestFixtures/tiny.obj", mesh);
    mesh.reset();
    Require(assets.FindMesh("Assets/TestFixtures/tiny.obj") != nullptr,
            "mesh cache released its strong handle");

    auto texture = std::make_shared<SE::Texture2D>();
    assets.CacheTexture(L"Assets/TestFixtures/texture.dds", texture);
    texture.reset();
    Require(assets.FindTexture(L"Assets/TestFixtures/texture.dds") != nullptr,
            "texture cache released its strong handle");

    const auto snapshot = assets.CaptureSnapshot();
    Require(snapshot.meshes.size() == 1 && snapshot.textures.size() == 1,
            "persistent assets were omitted from the cache snapshot");
}

static bool HasErrorAt(const SE::LevelAssetLoadResult& result, const char* path)
{
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.severity == LevelDiagnosticSeverity::Error && diagnostic.path == path)
            return true;
    return false;
}

static json ScriptedEntityLevel(json scripts, bool actor = true)
{
    json entity = json::object({
        { "id", "0000000000000001" },
        { "name", "Spawner" },
        { "components", json::object() }
    });
    if (actor)
        entity["actor"] = "Assets/Scripts/Actors/Spawner.lua";
    entity["scripts"] = std::move(scripts);
    json level = json::object({
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "Scripts" }
    });
    level["entities"] = json::array({ std::move(entity) });
    return level;
}

static void TestScriptParsing()
{
    {
        const auto result = LevelAssetLoader::Parse(ScriptedEntityLevel(json::array({
            json::object({ { "script", "Assets\\Scripts\\Spinner.lua" },
                           { "properties", json::object({ { "speed", 45 } }) } }),
            json::object({ { "script", "Assets/Scripts/Other.lua" } })
        })), "scripts");
        Require(result.Succeeded(), "valid scripts should parse");
        const auto& scripts = result.asset.entities[0].scripts;
        Require(scripts.size() == 2, "script count mismatch");
        Require(scripts[0].script == "Assets/Scripts/Spinner.lua",
                "script path backslashes were not normalised");
        Require(scripts[0].properties.value("speed", 0) == 45, "script property override lost");
        Require(scripts[1].properties.is_object() && scripts[1].properties.empty(),
                "omitted script properties should default to an empty object");
        Require(result.asset.dependencies.empty(), "scripts must not become asset dependencies");
    }
    {
        const auto result = LevelAssetLoader::Parse(ScriptedEntityLevel(json::array({
            json::object({ { "properties", json::object() } })
        })), "scripts");
        Require(HasErrorAt(result, "/entities/0/scripts/0/script"),
                "missing script path was not reported");
    }
    {
        const auto result = LevelAssetLoader::Parse(ScriptedEntityLevel(json::array({
            json::object({ { "script", "Assets/Scripts/Spinner.txt" } })
        })), "scripts");
        Require(HasErrorAt(result, "/entities/0/scripts/0/script") &&
                HasErrorContaining(result, ".lua"),
                "non-.lua script path was not rejected");
    }
    {
        const auto result = LevelAssetLoader::Parse(ScriptedEntityLevel(json::array({
            json::object({ { "script", "Assets/Scripts/Spinner.lua" },
                           { "properties", json::array({ 1, 2 }) } })
        })), "scripts");
        Require(HasErrorAt(result, "/entities/0/scripts/0/properties"),
                "non-object script properties were not rejected");
    }
    {
        const auto result = LevelAssetLoader::Parse(
            ScriptedEntityLevel(json::object({ { "script", "Assets/Scripts/Spinner.lua" } })),
            "scripts");
        Require(HasErrorAt(result, "/entities/0/scripts"), "non-array scripts were not rejected");
    }
    {
        // Static entities have no behaviour: scripts and properties belong to actors.
        const auto result = LevelAssetLoader::Parse(ScriptedEntityLevel(json::array({
            json::object({ { "script", "Assets/Scripts/Spinner.lua" } })
        }), false), "scripts");
        Require(HasErrorAt(result, "/entities/0/scripts"), "scripts on a static entity were accepted");
        json level = ScriptedEntityLevel(json::array(), false);
        level["entities"][0].erase("scripts");
        level["entities"][0]["properties"] = json::object({ { "speed", 1 } });
        Require(HasErrorAt(LevelAssetLoader::Parse(level, "scripts"), "/entities/0/properties"),
                "properties on a static entity were accepted");
    }
}

static json ScriptArray(const char* path)
{
    return json::array({ json::object({ { "script", path } }) });
}

static void TestDescriptorStamping()
{
    auto entity = [](const char* id, const char* name, json components) {
        return json::object({
            { "id", id }, { "name", name }, { "components", std::move(components) }
        });
    };
    json level = json::object({ { "format", "FoxLevel" }, { "version", 2 }, { "name", "Stamp" } });
    level["entities"] = json::array({
        entity("0000000000000011", "Statue", json::object({
            { "transform", json::object({ { "position", { 1.0f, 0.0f, 0.0f } } }) },
            { "meshRenderer", json::object({ { "mesh", "Assets/Test/statue.obj" } }) }
        })),
        entity("0000000000000012", "Main Camera",
               json::object({ { "camera", json::object({ { "primary", true } }) } })),
        entity("0000000000000013", "Key Light",
               json::object({ { "directionalLight", json::object() } })),
        entity("0000000000000014", "Lamp", json::object({ { "pointLight", json::object() } })),
        entity("0000000000000015", "Sparks",
               json::object({ { "particleEmitter", json::object() } })),
        entity("0000000000000016", "Ball", json::object({ { "sphereRenderer", json::object() } })),
        entity("0000000000000017", "Spawn", json::object({
            { "transform", json::object({
                { "position", { 1.0f, 2.0f, 3.0f } },
                { "rotationDegrees", { 0.0f, 90.0f, 0.0f } },
                { "scale", { 2.0f, 2.0f, 2.0f } }
            }) }
        })),
        entity("0000000000000018", "Bare", json::object())
    });
    json inactive = entity("0000000000000019", "Hidden", json::object());
    inactive["active"] = false;
    level["entities"].push_back(std::move(inactive));
    json door = entity("000000000000001A", "Door", json::object({
        { "transform", json::object({ { "position", { 4.0f, 0.0f, 1.0f } } }) },
        { "body", json::object({ { "mass", 5.0f } }) }
    }));
    door["actor"] = "Assets/Scripts/Actors/Door.lua";
    door["properties"] = json::object({ { "openAngle", 90 } });
    door["scripts"] = ScriptArray("Assets/Scripts/Creak.lua");
    level["entities"].push_back(std::move(door));

    const auto result = LevelAssetLoader::Parse(level, "stamping");
    Require(result.Succeeded(), "stamping level should parse");
    SE::SceneDescriptor desc;
    Require(SE::SceneLoader::BuildDescriptor(result.asset, desc), "stamping descriptor failed");

    Require(desc.meshes.size() == 1 && desc.meshes[0].sourceEntityID == 0x11,
            "mesh entry was not stamped");
    Require(desc.camera.sourceEntityID == 0x12 && desc.camera.name == "Main Camera",
            "camera entry was not stamped");
    Require(desc.sun.sourceEntityID == 0x13 && desc.sun.name == "Key Light",
            "sun entry was not stamped");
    Require(desc.pointLights.size() == 1 && desc.pointLights[0].sourceEntityID == 0x14 &&
            desc.pointLights[0].name == "Lamp", "point light entry was not stamped");
    Require(desc.particles.size() == 1 && desc.particles[0].sourceEntityID == 0x15 &&
            desc.particles[0].name == "Sparks", "particle emitter entry was not stamped");
    Require(desc.objects.size() == 1 && desc.objects[0].sourceEntityID == 0x16 &&
            desc.objects[0].name == "Ball", "primitive entry was not stamped");

    Require(desc.empties.size() == 2, "active component-less entities should become empties");
    const auto& spawn = desc.empties[0];
    Require(spawn.sourceEntityID == 0x17 && spawn.name == "Spawn", "empty entry was not stamped");
    Require(spawn.position == std::array<float, 3>{ 1.0f, 2.0f, 3.0f } &&
            spawn.rotation == std::array<float, 3>{ 0.0f, 90.0f, 0.0f } &&
            spawn.scale == std::array<float, 3>{ 2.0f, 2.0f, 2.0f },
            "empty transform was not copied");
    Require(desc.empties[1].sourceEntityID == 0x18, "bare empty entity mismatch");

    Require(desc.actors.size() == 1, "actor entity should become one actor entry");
    const auto& actor = desc.actors[0];
    Require(actor.sourceEntityID == 0x1A && actor.name == "Door" &&
            actor.actorClass == "Assets/Scripts/Actors/Door.lua",
            "actor entry was not stamped");
    Require(actor.position == std::array<float, 3>{ 4.0f, 0.0f, 1.0f },
            "actor root transform was not copied");
    Require(actor.components.contains("body") && !actor.components.contains("transform") &&
            actor.components["body"].value("mass", 0.0f) == 5.0f,
            "actor component overrides were not kept");
    Require(actor.properties.value("openAngle", 0) == 90, "actor properties were not kept");
    Require(actor.scripts.size() == 1 && actor.scripts[0].script == "Assets/Scripts/Creak.lua",
            "actor scripts were not kept");
}

static void TestSaverPreservesIdentity()
{
    SE::SceneDescriptor desc;
    desc.name = "Identity";

    SE::LevelScriptDefinition spinner;
    spinner.script = "Assets/Scripts/Spinner.lua";
    spinner.properties = json::object({ { "speed", 45 } });
    SE::LevelScriptDefinition plain;
    plain.script = "Assets/Scripts/Plain.lua";

    SE::SceneDescriptor::MeshEntry mesh;
    mesh.sourceEntityID = 0x10;
    mesh.name = "Statue";
    mesh.path = "Assets/Test/statue.obj";
    desc.meshes.push_back(mesh);
    mesh.sourceEntityID = 0;
    mesh.name = "Statue Copy";
    desc.meshes.push_back(mesh);

    desc.camera.sourceEntityID = 0x20;
    desc.sun.name = "Key Light";

    SE::SceneDescriptor::PointLightDesc light;
    light.sourceEntityID = 0x30;
    light.name = "Lamp";
    desc.pointLights.push_back(light);
    light.name.clear(); // same id again: must not be written twice
    desc.pointLights.push_back(light);

    SE::SceneDescriptor::SceneObject object;
    object.name = "Ball";
    desc.objects.push_back(object);

    SE::SceneDescriptor::ParticleEmitterDesc emitter;
    emitter.sourceEntityID = 0x40;
    emitter.name = "Sparks";
    desc.particles.push_back(emitter);

    SE::SceneDescriptor::EmptyEntity empty;
    empty.sourceEntityID = 0x50;
    empty.name = "Spawn";
    empty.position = { 1.0f, 2.0f, 3.0f };
    empty.rotation = { 0.0f, 45.0f, 0.0f };
    empty.scale = { 1.0f, 2.0f, 1.0f };
    desc.empties.push_back(empty);
    SE::SceneDescriptor::EmptyEntity nameless = empty;
    nameless.sourceEntityID = 0;
    nameless.name.clear();
    desc.empties.push_back(nameless);

    SE::SceneDescriptor::ActorEntry actor;
    actor.sourceEntityID = 0x45;
    actor.name = "Door";
    actor.actorClass = "Assets/Scripts/Actors/Door.lua";
    actor.position = { 4.0f, 0.0f, 1.0f };
    actor.components = json::object({ { "body", json::object({ { "mass", 5.0f } }) } });
    actor.properties = json::object({ { "openAngle", 90 } });
    actor.scripts = { spinner, plain };
    desc.actors.push_back(actor);
    SE::SceneDescriptor::ActorEntry bareActor;
    bareActor.actorClass = "Assets/Scripts/Actors/Door.lua";
    desc.actors.push_back(bareActor);

    const std::string text = SE::SceneSaver::ToJson(desc);
    const json saved = json::parse(text);
    const auto reparsed = LevelAssetLoader::Parse(saved, "identity");
    Require(reparsed.Succeeded(), "identity save failed validation");

    std::set<SE::LevelEntityID> ids;
    for (const auto& entity : reparsed.asset.entities)
        ids.insert(entity.id);
    Require(ids.size() == reparsed.asset.entities.size(), "saved entity ids are not unique");

    for (const auto& entity : saved["entities"])
    {
        if (!entity.contains("actor"))
            Require(!entity.contains("scripts") && !entity.contains("properties"),
                    "static entities must not save behaviour");
        if (entity["name"] == "Actor")
            Require(!entity.contains("scripts") && !entity.contains("properties"),
                    "empty actor scripts/properties should be omitted");
        if (!entity.contains("scripts"))
            continue;
        for (const auto& script : entity["scripts"])
            if (script["script"] == "Assets/Scripts/Plain.lua")
                Require(!script.contains("properties"),
                        "empty script properties should be omitted");
    }

    SE::SceneDescriptor loaded;
    Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded), "identity rebuild failed");
    Require(loaded.meshes.size() == 2 && loaded.meshes[0].sourceEntityID == 0x10,
            "mesh id not preserved");
    Require(loaded.meshes[1].sourceEntityID > 0x50, "zero mesh id should get a fresh id");
    Require(loaded.camera.sourceEntityID == 0x20 && loaded.camera.name == "Camera",
            "camera id/name not preserved");
    Require(loaded.sun.sourceEntityID > 0x50 && loaded.sun.name == "Key Light",
            "sun name not preserved");
    Require(loaded.pointLights.size() == 2 && loaded.pointLights[0].sourceEntityID == 0x30 &&
            loaded.pointLights[0].name == "Lamp", "point light id/name not preserved");
    Require(loaded.pointLights[1].sourceEntityID > 0x50 &&
            loaded.pointLights[1].name == "Point Light 2",
            "repeated point light id should get a fresh id");
    Require(loaded.objects.size() == 1 && loaded.objects[0].sourceEntityID > 0x50,
            "zero object id should get a fresh id");
    Require(loaded.particles.size() == 1 && loaded.particles[0].sourceEntityID == 0x40,
            "emitter id not preserved");
    Require(loaded.empties.size() == 2, "empties were not saved");
    Require(loaded.empties[0].sourceEntityID == 0x50 && loaded.empties[0].name == "Spawn" &&
            loaded.empties[0].position == empty.position &&
            loaded.empties[0].rotation == empty.rotation &&
            loaded.empties[0].scale == empty.scale,
            "empty entity not preserved");
    Require(loaded.empties[1].sourceEntityID > 0x50 && loaded.empties[1].name == "Empty",
            "nameless empty should save as 'Empty' with a fresh id");
    Require(loaded.actors.size() == 2, "actors were not saved");
    const auto& door = loaded.actors[0];
    Require(door.sourceEntityID == 0x45 && door.name == "Door" &&
            door.actorClass == actor.actorClass && door.position == actor.position &&
            door.components == actor.components && door.properties == actor.properties,
            "actor placement not preserved");
    Require(door.scripts.size() == 2 &&
            door.scripts[0].script == "Assets/Scripts/Spinner.lua" &&
            door.scripts[0].properties.value("speed", 0) == 45 &&
            door.scripts[1].script == "Assets/Scripts/Plain.lua",
            "actor scripts not preserved");
    Require(loaded.actors[1].sourceEntityID > 0x50 && loaded.actors[1].name == "Actor",
            "zero actor id should get a fresh id");

    Require(SE::SceneSaver::ToJson(loaded) == text, "identity serialization is not stable");
}

static void TestLevelWorldComponentPositions()
{
    json level = json::object({ { "format", "FoxLevel" }, { "version", 2 }, { "name", "World" } });
    json root = json::object({
        { "id", "0000000000000001" }, { "name", "Root" },
        { "components", json::object({ { "transform", json::object({
            { "position", { 10.0f, 0.0f, 0.0f } },
            { "scale", { 2.0f, 2.0f, 2.0f } }
        }) } }) }
    });
    json child = json::object({
        { "id", "0000000000000002" }, { "parent", "0000000000000001" }, { "name", "Lamp" },
        { "components", json::object({ { "pointLight", json::object({
            { "position", { 12.0f, 4.0f, 0.0f } }
        }) } }) }
    });
    json loose = json::object({
        { "id", "0000000000000003" }, { "name", "Loose" },
        { "components", json::object({ { "pointLight", json::object({
            { "position", { 5.0f, 6.0f, 7.0f } }
        }) } }) }
    });
    level["entities"] = json::array({ root, child, loose });

    const auto result = LevelAssetLoader::Parse(level, "world");
    Require(result.Succeeded(), "world level should parse");
    SE::LevelWorld world;
    Require(world.Instantiate(result.asset), "world level failed to instantiate");

    SE::Entity* rootEntity = world.FindBySourceID(1);
    SE::Entity* lampEntity = world.FindBySourceID(2);
    SE::Entity* looseEntity = world.FindBySourceID(3);
    Require(rootEntity && lampEntity && looseEntity, "world entities missing");

    auto nearly = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };
    const auto* looseTransform = looseEntity->GetComponent<SE::TransformComponent>();
    Require(nearly(looseTransform->position.x, 5.0f) && nearly(looseTransform->position.y, 6.0f) &&
            nearly(looseTransform->position.z, 7.0f),
            "point light position did not initialise the transform");

    const auto* lampTransform = lampEntity->GetComponent<SE::TransformComponent>();
    Require(nearly(lampTransform->position.x, 1.0f) && nearly(lampTransform->position.y, 2.0f) &&
            nearly(lampTransform->position.z, 0.0f),
            "parented point light position was not converted into parent space");
    DirectX::XMFLOAT3 lampWorld;
    DirectX::XMStoreFloat3(&lampWorld, lampTransform->GetWorldMatrix().r[3]);
    Require(nearly(lampWorld.x, 12.0f) && nearly(lampWorld.y, 4.0f) && nearly(lampWorld.z, 0.0f),
            "parented point light world position changed");
}

static void TestInMemoryLevelLoad()
{
    std::ifstream file("async_level.json", std::ios::binary);
    Require(file.is_open(), "async level fixture missing");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    {
        SE::LevelLoadOperation operation;
        operation.Start("not_on_disk.json", {}, text);
        const auto status = WaitForCpuPreparation(operation);
        Require(status.state == SE::LevelLoadState::Uploading,
                "in-memory level text was not prepared");
        operation.Cancel();
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("async_level.json", {}, "{ not json");
        const auto status = WaitForCpuPreparation(operation);
        Require(status.state == SE::LevelLoadState::Failed,
                "malformed in-memory level text should fail");
    }
}

static void TestActorShapeComponents()
{
    SE::ActorComponentDefinition def;
    std::string error;
    Require(SE::ParseActorComponent(json{ { "name", "body" }, { "type", "box" } }, def, error),
            "minimal box should parse");
    Require(def.visible && def.castShadows && def.color[0] == 0.8f, "box should default to a visible grey shape");

    Require(SE::ParseActorComponent(json{ { "name", "trigger" }, { "type", "sphere" }, { "visible", false },
                                          { "color", { 0.1, 0.2, 0.3 } }, { "cast_shadows", false } },
                                    def, error),
            "sphere render keys should parse");
    Require(!def.visible && !def.castShadows && def.color[2] == 0.3f, "sphere render keys were not applied");

    Require(!SE::ParseActorComponent(json{ { "name", "body" }, { "type", "box" }, { "color", { 2, 1, 1 } } },
                                     def, error) && error.find("'color'") != std::string::npos,
            "a box albedo above 1 should be rejected");
    Require(!SE::ParseActorComponent(json{ { "name", "ember" }, { "type", "mesh" }, { "mesh", "a.obj" },
                                           { "visible", true } }, def, error),
            "visible is not a mesh key");

    Require(SE::ParseActorComponent(json{ { "name", "body" }, { "type", "box" } }, def, error) &&
                def.collision && def.emissive[0] == 0.0f,
            "a box should collide and not glow by default");
    Require(SE::ParseActorComponent(json{ { "name", "barrel" }, { "type", "box" }, { "collision", false },
                                          { "emissive", { 0.5, 4, 8 } } },
                                    def, error) &&
                !def.collision && def.emissive[2] == 8.0f,
            "a render-only box with HDR emissive should parse");
    Require(SE::ParseActorComponent(json{ { "name", "ember" }, { "type", "mesh" }, { "mesh", "a.obj" } }, def, error) &&
                !def.collision,
            "a mesh should not collide by default");
    Require(!SE::ParseActorComponent(json{ { "name", "body" }, { "type", "sphere" }, { "emissive", { -1, 0, 0 } } },
                                     def, error) && error.find("'emissive'") != std::string::npos,
            "negative emissive should be rejected");
    Require(!SE::ParseActorComponent(json{ { "name", "body" }, { "type", "sphere" }, { "body", "dynamic" },
                                           { "collision", false } },
                                     def, error) && error.find("collision") != std::string::npos,
            "a dynamic body without collision should be rejected");
    Require(!SE::ParseActorComponent(json{ { "name", "glow" }, { "type", "point_light" }, { "emissive", { 1, 1, 1 } } },
                                     def, error),
            "emissive is not a point_light key");

    // Back-face culling hides the box from outside if any face winds the wrong way.
    std::vector<SE::MeshVertex> vertices;
    std::vector<uint32_t> indices;
    SE::BuildBoxGeometry({ 1.0f, 2.0f, 3.0f }, vertices, indices);
    Require(indices.size() == 36, "a box has 12 triangles");
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        const auto at = [&](size_t k) {
            const SE::MeshVertex& v = vertices[indices[i + k]];
            return DirectX::XMVectorSet(v.x, v.y, v.z, 0.0f);
        };
        const DirectX::XMVECTOR p0 = at(0), p1 = at(1), p2 = at(2);
        const DirectX::XMVECTOR facing = DirectX::XMVector3Cross(DirectX::XMVectorSubtract(p1, p0),
                                                                 DirectX::XMVectorSubtract(p2, p0));
        const DirectX::XMVECTOR centroid = DirectX::XMVectorScale(DirectX::XMVectorAdd(DirectX::XMVectorAdd(p0, p1), p2),
                                                                  1.0f / 3.0f);
        Require(DirectX::XMVectorGetX(DirectX::XMVector3Dot(facing, centroid)) > 0.0f,
                "a box triangle faces inward");
    }
}

static void RunTests()
{
    TestAssetCacheRetainsReleasedHandles();
    TestValidV2();
    TestValidationFailures();
    TestTypedFieldValidation();
    TestPlanarReflectionMaterial();
    TestAtmosphereSchema();
    TestLegacyConversion();
    TestRepositoryScenes();
    TestSaveRoundTrip();
    TestShadowDistanceRoundTrip();
    TestMotionBlurRoundTrip();
    TestReflectionProbeSchema();
    TestBistroRuntimeDescriptor();
    TestScriptParsing();
    TestDescriptorStamping();
    TestSaverPreservesIdentity();
    TestLevelWorldComponentPositions();
    TestActorShapeComponents();
    TestDependencyPlan();
    TestMaterialTexturePath();
    TestCpuTextureDecode();
    TestGltfImporterParity({ "Assets/TestAssets/SpecGlossVsMetalRough/SpecGlossVsMetalRough.gltf",
                             "Assets/Vehicles/Porsche911GT3R/body.gltf" });
    std::filesystem::current_path(FOX_TEST_FIXTURE_ROOT);
    TestOperationFailuresAndCancellation();
    TestCachedMeshSkipsImport();
    TestInMemoryLevelLoad();
    TestCpuMeshImport();
    TestGltfPackedMRDetection();
    TestGltfSpecGlossDetection();
    TestGltfImporterParity({ "Assets/TestFixtures/packed_mr.gltf", "Assets/TestFixtures/spec_gloss.gltf",
                             "Assets/TestFixtures/slots.gltf" });
    TestMeshCache();
    TestVertexStreams();
    TestSubMeshBounds();
    TestTextureSlots();
    TestVertexTint();
    TestTerrainMaterial();
    TestClearcoat();
    TestWorldUVAndDetailNeutral();
    std::cout << "LevelAssetTests: all tests passed\n";
}

int main()
{
    try { RunTests(); }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    return 0;
}
