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

#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>
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
    Require(source.subMeshes[0].material.albedoPath == "missing_albedo.png",
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
    Require(packed.albedoPath == "albedo.png", "glTF base color path mismatch");
    Require(packed.roughnessPath == "packed_mr.png",
            "glTF metallic-roughness texture was not routed to the roughness slot");
    Require(packed.workflow == SE::MaterialWorkflow::PackedMR,
            "shared metallic-roughness texture was not flagged as packed");
    Require(packed.normalPath == "normal.png", "glTF normal path mismatch");
    Require(packed.roughnessFactor == 1.0f, "textured material roughness factor should default to 1");

    const auto& scalar = source.subMeshes[1].material;
    Require(scalar.roughnessPath.empty(), "texture-less material should have no roughness path");
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
    Require(textured.albedoPath == "diffuse.png", "spec/gloss diffuse path mismatch");
    Require(textured.roughnessPath == "spec_gloss.png",
            "specular-glossiness texture was not routed to the roughness slot");
    Require(textured.workflow == SE::MaterialWorkflow::SpecularGloss,
            "spec/gloss material was not detected");
    Require(textured.glossinessFactor > 0.59f && textured.glossinessFactor < 0.61f,
            "glossiness factor was not imported");

    const auto& scalar = source.subMeshes[1].material;
    Require(scalar.workflow == SE::MaterialWorkflow::SeparateMR,
            "texture-less spec/gloss should fall back to the scalar MR path");
    Require(scalar.roughnessPath.empty(), "texture-less spec/gloss should have no roughness path");
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

static bool SameMaterial(const SE::SubMeshInfo& a, const SE::SubMeshInfo& b)
{
    return a.albedoPath == b.albedoPath && a.normalPath == b.normalPath &&
           a.roughnessPath == b.roughnessPath && a.emissivePath == b.emissivePath &&
           a.alphaMode == b.alphaMode && a.alphaCutoff == b.alphaCutoff &&
           a.workflow == b.workflow && a.roughnessFactor == b.roughnessFactor &&
           a.glossinessFactor == b.glossinessFactor;
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
    Require(cooked.subMeshes.size() == 1 && cooked.subMeshes[0].material.albedoPath == "missing_albedo.png",
            "cooked-only load lost material data");
    Require(cache.Cook(source, false) == SE::MeshCookResult::Failed, "cooking a missing source succeeded");

    SE::AssetManager assets;
    assets.CacheMesh(source, std::make_shared<SE::Mesh>());
    Require(assets.FindMesh(source) != nullptr, "mesh with a missing source was evicted from the asset cache");

    fs::remove_all(root);
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

static json ScriptedEntityLevel(json scripts)
{
    json entity = json::object({
        { "id", "0000000000000001" },
        { "name", "Spawner" },
        { "components", json::object() }
    });
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
}

static json ScriptArray(const char* path)
{
    return json::array({ json::object({ { "script", path } }) });
}

static void TestDescriptorStamping()
{
    auto entity = [](const char* id, const char* name, json components, const char* script) {
        json out = json::object({
            { "id", id }, { "name", name }, { "components", std::move(components) }
        });
        if (script)
            out["scripts"] = ScriptArray(script);
        return out;
    };
    json level = json::object({ { "format", "FoxLevel" }, { "version", 2 }, { "name", "Stamp" } });
    level["entities"] = json::array({
        entity("0000000000000011", "Statue", json::object({
            { "transform", json::object({ { "position", { 1.0f, 0.0f, 0.0f } } }) },
            { "meshRenderer", json::object({ { "mesh", "Assets/Test/statue.obj" } }) }
        }), "Assets/Scripts/Mesh.lua"),
        entity("0000000000000012", "Main Camera",
               json::object({ { "camera", json::object({ { "primary", true } }) } }),
               "Assets/Scripts/Camera.lua"),
        entity("0000000000000013", "Key Light",
               json::object({ { "directionalLight", json::object() } }),
               "Assets/Scripts/Sun.lua"),
        entity("0000000000000014", "Lamp",
               json::object({ { "pointLight", json::object() } }),
               "Assets/Scripts/Lamp.lua"),
        entity("0000000000000015", "Sparks",
               json::object({ { "particleEmitter", json::object() } }),
               "Assets/Scripts/Sparks.lua"),
        entity("0000000000000016", "Ball",
               json::object({ { "sphereRenderer", json::object() } }),
               "Assets/Scripts/Ball.lua"),
        entity("0000000000000017", "Spawn", json::object({
            { "transform", json::object({
                { "position", { 1.0f, 2.0f, 3.0f } },
                { "rotationDegrees", { 0.0f, 90.0f, 0.0f } },
                { "scale", { 2.0f, 2.0f, 2.0f } }
            }) }
        }), "Assets/Scripts/Spawn.lua"),
        entity("0000000000000018", "Bare", json::object(), nullptr)
    });
    json inactive = entity("0000000000000019", "Hidden", json::object(), nullptr);
    inactive["active"] = false;
    level["entities"].push_back(std::move(inactive));

    const auto result = LevelAssetLoader::Parse(level, "stamping");
    Require(result.Succeeded(), "stamping level should parse");
    SE::SceneDescriptor desc;
    Require(SE::SceneLoader::BuildDescriptor(result.asset, desc), "stamping descriptor failed");

    auto hasScript = [](const std::vector<SE::LevelScriptDefinition>& scripts, const char* path) {
        return scripts.size() == 1 && scripts[0].script == path;
    };
    Require(desc.meshes.size() == 1 && desc.meshes[0].sourceEntityID == 0x11 &&
            hasScript(desc.meshes[0].scripts, "Assets/Scripts/Mesh.lua"),
            "mesh entry was not stamped");
    Require(desc.camera.sourceEntityID == 0x12 && desc.camera.name == "Main Camera" &&
            hasScript(desc.camera.scripts, "Assets/Scripts/Camera.lua"),
            "camera entry was not stamped");
    Require(desc.sun.sourceEntityID == 0x13 && desc.sun.name == "Key Light" &&
            hasScript(desc.sun.scripts, "Assets/Scripts/Sun.lua"),
            "sun entry was not stamped");
    Require(desc.pointLights.size() == 1 && desc.pointLights[0].sourceEntityID == 0x14 &&
            desc.pointLights[0].name == "Lamp" &&
            hasScript(desc.pointLights[0].scripts, "Assets/Scripts/Lamp.lua"),
            "point light entry was not stamped");
    Require(desc.particles.size() == 1 && desc.particles[0].sourceEntityID == 0x15 &&
            desc.particles[0].name == "Sparks" &&
            hasScript(desc.particles[0].scripts, "Assets/Scripts/Sparks.lua"),
            "particle emitter entry was not stamped");
    Require(desc.objects.size() == 1 && desc.objects[0].sourceEntityID == 0x16 &&
            desc.objects[0].name == "Ball" &&
            hasScript(desc.objects[0].scripts, "Assets/Scripts/Ball.lua"),
            "primitive entry was not stamped");

    Require(desc.empties.size() == 2, "active component-less entities should become empties");
    const auto& spawn = desc.empties[0];
    Require(spawn.sourceEntityID == 0x17 && spawn.name == "Spawn" &&
            hasScript(spawn.scripts, "Assets/Scripts/Spawn.lua"),
            "empty entry was not stamped");
    Require(spawn.position == std::array<float, 3>{ 1.0f, 2.0f, 3.0f } &&
            spawn.rotation == std::array<float, 3>{ 0.0f, 90.0f, 0.0f } &&
            spawn.scale == std::array<float, 3>{ 2.0f, 2.0f, 2.0f },
            "empty transform was not copied");
    Require(desc.empties[1].sourceEntityID == 0x18 && desc.empties[1].scripts.empty(),
            "bare empty entity mismatch");
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
    mesh.scripts = { spinner };
    desc.meshes.push_back(mesh);
    mesh.sourceEntityID = 0;
    mesh.name = "Statue Copy";
    desc.meshes.push_back(mesh);

    desc.camera.sourceEntityID = 0x20;
    desc.camera.scripts = { plain };
    desc.sun.name = "Key Light";

    SE::SceneDescriptor::PointLightDesc light;
    light.sourceEntityID = 0x30;
    light.name = "Lamp";
    light.scripts = { spinner };
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
    empty.scripts = { plain };
    desc.empties.push_back(empty);
    SE::SceneDescriptor::EmptyEntity nameless = empty;
    nameless.sourceEntityID = 0;
    nameless.name.clear();
    nameless.scripts.clear();
    desc.empties.push_back(nameless);

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
        if (entity["name"] == "Ball" || entity["name"] == "Sparks")
            Require(!entity.contains("scripts"), "empty scripts arrays should be omitted");
        if (!entity.contains("scripts"))
            continue;
        for (const auto& script : entity["scripts"])
            if (script["script"] == "Assets/Scripts/Plain.lua")
                Require(!script.contains("properties"),
                        "empty script properties should be omitted");
    }

    SE::SceneDescriptor loaded;
    Require(SE::SceneLoader::BuildDescriptor(reparsed.asset, loaded), "identity rebuild failed");
    Require(loaded.meshes.size() == 2 && loaded.meshes[0].sourceEntityID == 0x10 &&
            loaded.meshes[0].scripts.size() == 1 &&
            loaded.meshes[0].scripts[0].script == "Assets/Scripts/Spinner.lua" &&
            loaded.meshes[0].scripts[0].properties.value("speed", 0) == 45,
            "mesh id/scripts not preserved");
    Require(loaded.meshes[1].sourceEntityID > 0x50, "zero mesh id should get a fresh id");
    Require(loaded.camera.sourceEntityID == 0x20 && loaded.camera.name == "Camera" &&
            loaded.camera.scripts.size() == 1, "camera id/name/scripts not preserved");
    Require(loaded.sun.sourceEntityID > 0x50 && loaded.sun.name == "Key Light",
            "sun name not preserved");
    Require(loaded.pointLights.size() == 2 && loaded.pointLights[0].sourceEntityID == 0x30 &&
            loaded.pointLights[0].name == "Lamp" && loaded.pointLights[0].scripts.size() == 1,
            "point light id/name/scripts not preserved");
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
            loaded.empties[0].scale == empty.scale &&
            loaded.empties[0].scripts.size() == 1 &&
            loaded.empties[0].scripts[0].script == "Assets/Scripts/Plain.lua",
            "empty entity not preserved");
    Require(loaded.empties[1].sourceEntityID > 0x50 && loaded.empties[1].name == "Empty",
            "nameless empty should save as 'Empty' with a fresh id");

    Require(SE::SceneSaver::ToJson(loaded) == text, "identity serialization is not stable");
}

static void TestLevelWorldScriptsAndComponentPositions()
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
    child["scripts"] = ScriptArray("Assets/Scripts/Flicker.lua");
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
    Require(!rootEntity->GetComponent<SE::ScriptComponent>(),
            "script component attached to an entity without scripts");
    const auto* scripts = lampEntity->GetComponent<SE::ScriptComponent>();
    Require(scripts && scripts->scripts.size() == 1 &&
            scripts->scripts[0].script == "Assets/Scripts/Flicker.lua",
            "script component missing from scripted entity");

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
    TestBistroRuntimeDescriptor();
    TestScriptParsing();
    TestDescriptorStamping();
    TestSaverPreservesIdentity();
    TestLevelWorldScriptsAndComponentPositions();
    TestDependencyPlan();
    TestMaterialTexturePath();
    TestCpuTextureDecode();
    std::filesystem::current_path(FOX_TEST_FIXTURE_ROOT);
    TestOperationFailuresAndCancellation();
    TestCachedMeshSkipsImport();
    TestInMemoryLevelLoad();
    TestCpuMeshImport();
    TestGltfPackedMRDetection();
    TestGltfSpecGlossDetection();
    TestMeshCache();
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
