#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/TransformComponent.h"

#include <iostream>
#include <stdexcept>
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
                { "reflectionPanorama", "Assets/Bistro/san_giuseppe_bridge_4k.dds" }
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
}

static void TestTypedFieldValidation()
{
    const json invalid = {
        { "format", "FoxLevel" },
        { "version", 2 },
        { "name", "Bad fields" },
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
        "Assets/Scenes/ssr_test.json",
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
}

int main()
{
    TestValidV2();
    TestValidationFailures();
    TestTypedFieldValidation();
    TestLegacyConversion();
    TestRepositoryScenes();
    TestBistroRuntimeDescriptor();
    std::cout << "LevelAssetTests: all tests passed\n";
    return 0;
}
