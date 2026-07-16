#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace SE {

struct LevelTransformDefinition
{
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> rotationDegrees = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
};

struct LevelMeshRendererDefinition
{
    std::string mesh;
    float alphaCutoff = 0.0f;
    bool castShadows = true;
    bool receiveShadows = true;
};

struct LevelCameraDefinition
{
    std::array<float, 3> eye = { 0.0f, 5.0f, -10.0f };
    float yaw = 0.0f;
    float pitch = 0.0f;
    float nearZ = 0.15f;
    float farZ = 4000.0f;
    bool primary = true;
};

struct LevelDirectionalLightDefinition
{
    float elevation = 63.0f;
    float azimuth = -134.6f;
    float intensity = 1.0f;
    std::array<float, 3> color = { 1.0f, 1.0f, 1.0f };
};

struct LevelPointLightDefinition
{
    std::array<float, 3> position = { 0.0f, 5.0f, 0.0f };
    std::array<float, 3> color = { 1.0f, 1.0f, 1.0f };
    float radius = 50.0f;
    bool castShadow = false;
    bool flicker = false;
    float flickerAmplitude = 0.15f;
    float flickerFrequency = 7.0f;
    uint32_t flickerSeed = 0;
};

struct LevelMaterialDefinition
{
    std::string albedo;
    std::string normal;
    std::string roughness;
    std::string metallic;
    std::string emissive;
    float emissiveIntensity = 1.0f;
    std::array<float, 3> emissiveColor = { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> tint = { 1.0f, 1.0f, 1.0f };
    bool planarReflection = false;
    float reflectionStrength = 1.0f;
    float reflectionResolutionScale = 0.5f;
    float roughnessScale = 1.0f;
    float metallicScale = 1.0f;
};

enum class LevelPrimitiveType { Sphere, Plane };

struct LevelPrimitiveRendererDefinition
{
    LevelPrimitiveType type = LevelPrimitiveType::Sphere;
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    float radius = 1.0f;
    float halfSizeX = 10.0f;
    float halfSizeZ = 10.0f;
    LevelMaterialDefinition material;
};

struct LevelParticleEmitterDefinition
{
    std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
    float emitRate = 50.0f;
    int maxParticles = 1000;
    float lifetimeMin = 1.0f;
    float lifetimeMax = 3.0f;
    std::array<float, 3> velocityMin = { -1.0f, 1.0f, -1.0f };
    std::array<float, 3> velocityMax = { 1.0f, 5.0f, 1.0f };
    float sizeStart = 0.2f;
    float sizeEnd = 0.05f;
    std::array<float, 4> colorStart = { 1.0f, 0.8f, 0.2f, 1.0f };
    std::array<float, 4> colorEnd = { 1.0f, 0.2f, 0.0f, 0.0f };
    std::array<float, 3> gravity = { 0.0f, -2.0f, 0.0f };
    float spawnRadius = 0.0f;
    int spawnShape = 0; // 0=sphere, 1=box
    std::array<float, 3> spawnBoxExtents = { 1.0f, 1.0f, 1.0f };
    std::string texture;
    int atlasColumns = 1;
    int atlasRows = 1;
    int atlasFrameCount = 0;
    float atlasSpeed = 1.0f;
    float textureRotationDegrees = 0.0f;
    float softDistance = 0.5f;
    float aspectRatio = 1.0f;
    int blendMode = 0;
    float drag = 0.0f;
    int shaderMode = 0;
    int emitMode = 0;
    float rotationDegrees = 0.0f;
    bool randomRotation = true;
    float tracerStretch = 0.0f;
    float lightingInfluence = 0.0f;
};

} // namespace SE
