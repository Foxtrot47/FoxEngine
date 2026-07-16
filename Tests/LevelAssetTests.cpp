#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/LevelRuntimeComponents.h"
#include "Engine/Scene/TransformComponent.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Renderer/Mesh.h"
#include "Engine/Renderer/Texture2D.h"

#include <chrono>
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
    Require(source.subMeshes[0].vertices.size() == 3, "tiny OBJ vertex count mismatch");
    Require(source.subMeshes[0].material.albedoPath == "missing_albedo.png",
            "material texture dependency was not discovered");

    SE::MeshSourceData cancelled;
    Require(!SE::Mesh::Import("Assets/TestFixtures/tiny.obj", cancelled,
        [](float) { return false; }), "cancelled mesh import unexpectedly succeeded");
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
        operation.Start("Tests/Fixtures/async_level.json", {});
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
        operation.Start("Tests/Fixtures/missing_geometry.json", {});
        const auto status = WaitForCpuPreparation(operation);
        Require(status.state == SE::LevelLoadState::Failed,
                "required geometry failure did not reject preparation");
        Require(status.requiredFailures == 1, "required failure count mismatch");
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("Tests/Fixtures/async_level.json", {});
        operation.Cancel();
        Require(operation.GetStatus().state == SE::LevelLoadState::Cancelled,
                "active operation did not cancel cleanly");
    }
    {
        SE::LevelLoadOperation operation;
        operation.Start("Tests/Fixtures/async_level.json", {});
    }
}

static void TestCachedMeshSkipsImport()
{
    SE::AssetCacheSnapshot cache;
    cache.meshes.emplace("Assets/TestFixtures/not_here.obj", std::make_shared<SE::Mesh>());
    SE::LevelLoadOperation operation;
    operation.Start("Tests/Fixtures/missing_geometry.json", std::move(cache));
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

static void RunTests()
{
    TestOperationFailuresAndCancellation();
    TestCachedMeshSkipsImport();
    TestAssetCacheRetainsReleasedHandles();
    TestValidV2();
    TestValidationFailures();
    TestTypedFieldValidation();
    TestPlanarReflectionMaterial();
    TestAtmosphereSchema();
    TestLegacyConversion();
    TestRepositoryScenes();
    TestBistroRuntimeDescriptor();
    TestDependencyPlan();
    TestCpuMeshImport();
    TestCpuTextureDecode();
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
