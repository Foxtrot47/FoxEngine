#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include "Engine/Scene/LevelTypes.h"

namespace SE {

struct SceneDescriptor
{
    std::string name = "Untitled";

    // Mesh
    struct MeshEntry {
        uint64_t sourceEntityID = 0;
        std::string name = "Mesh";
        std::string path;
        std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> rotation = { 0.0f, 0.0f, 0.0f }; // euler degrees (pitch, yaw, roll)
        std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
        float alphaCutoff = 0.0f; // if > 0, force alpha-test on submeshes with alpha textures
        bool castShadows = true;
        bool receiveShadows = true;
    };
    std::vector<MeshEntry> meshes;

    // Skybox
    std::string skybox;

    // Optional equirectangular HDR panorama used for environment lighting.
    std::string reflectionPanorama;

    // Camera
    using CameraDesc = LevelCameraDefinition;
    CameraDesc camera;

    // Directional light (sun)
    using SunDesc = LevelDirectionalLightDefinition;
    SunDesc sun;

    float iblIntensity = 1.0f;

    // Point lights
    using PointLightDesc = LevelPointLightDefinition;
    std::vector<PointLightDesc> pointLights;

    // Physics
    struct PhysicsDesc {
        struct FloorDesc {
            std::array<float, 3> min = { -30.0f, -0.5f, -30.0f };
            std::array<float, 3> max = {  30.0f,  0.0f,  30.0f };
            float friction    = 0.6f;
            float restitution = 0.4f;
        };
        FloorDesc floor;
        std::array<float, 3> ballSpawn      = { 0.0f, 30.0f, 0.0f };
        float                ballRadius     = 1.0f;
        std::array<float, 3> characterSpawn = { 0.0f, 5.0f, 0.0f };
        bool                 gravity        = true;
    };
    PhysicsDesc physics;

    // Tone mapping
    struct ToneMappingDesc {
        int   op           = 1; // 0=Reinhard, 1=ACES
        float exposure     = 1.0f;
        bool  gammaCorrect = false;
    };
    ToneMappingDesc toneMapping;

    // Bloom
    struct BloomDesc {
        bool  enabled   = true;
        float threshold = 1.0f;
        float intensity = 0.1f;
        float scatter   = 0.7f;
    };
    BloomDesc bloom;

    // Volumetric fog
    struct FogDesc {
        bool  enabled          = false;
        float density          = 0.03f;
        float anisotropy       = 0.76f;
        float scatterIntensity = 1.0f;
        int   steps            = 48;
        float maxDistance      = 60.0f;
        float ambientColor[3]  = { 0.42f, 0.44f, 0.42f };
        float ambientIntensity = 0.45f;
        float heightFalloff    = 0.0f;
        float heightBase       = 0.0f;
        float noiseStrength    = 0.35f;
        float noiseScale       = 0.035f;
        float noiseSpeed       = 0.45f;
        float nearClearDistance = 0.0f;
        float distanceDensity   = 0.0f;
    };
    FogDesc fog;

    // Optional array of scene objects with explicit geometry and PBR material paths.
    // Rendered in addition to the main mesh. Enables a fully JSON-driven PBR showcase.
    struct SceneObject
    {
        enum class Type { Sphere, Plane } type = Type::Sphere;

        std::array<float, 3> position = { 0.f, 0.f, 0.f };
        float radius    = 1.0f;   // sphere radius
        float halfSizeX = 10.0f;  // plane half-extents (plane only)
        float halfSizeZ = 10.0f;

        // PBR material texture paths (DDS, relative to working dir)
        std::string albedoPath;
        std::string normalPath;
        std::string roughnessPath;
        std::string metallicPath;
        std::string emissivePath;
        float emissiveIntensity = 1.0f;
        std::array<float, 3> emissiveColor = { 1.f, 1.f, 1.f };

        std::array<float, 3> tint = { 1.f, 1.f, 1.f };
        bool planarReflection = false;
        float reflectionStrength = 1.0f;
        float reflectionResolutionScale = 0.5f;
        float roughnessScale = 1.0f;
        float metallicScale = 1.0f;
    };
    std::vector<SceneObject> objects;

    // Particle emitters
    using ParticleEmitterDesc = LevelParticleEmitterDefinition;
    std::vector<ParticleEmitterDesc> particles;
};

} // namespace SE
