#include <windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <memory>
#include <filesystem>
#include <cmath>
#include "Engine/Core/Engine.h"
#include "Engine/Core/Logger.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Physics/Plane.h"
#include "Engine/Physics/Ray.h"
#include "Engine/Physics/OBB.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneDescriptor.h"
#include "Engine/Scene/SceneLoader.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Scene/TransformComponent.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Scene/Camera/CameraController.h"
#include "Engine/Physics/RigidBodyComponent.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/LightEnvironment.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Renderer/SkyboxRenderer.h"
#include "Engine/Renderer/EnvironmentLighting.h"
#include "Engine/Renderer/CascadedShadowMap.h"
#include "Engine/Renderer/RenderTarget.h"
#include "Engine/Renderer/ToneMap.h"
#include "Engine/Renderer/PointShadowMap.h"
#include "Engine/Renderer/Bloom.h"
#include "Engine/Renderer/SSAO.h"
#include "Engine/Renderer/VolumetricFog.h"
#include "Engine/Renderer/FXAA.h"
#include "Engine/Renderer/ParticleSystem.h"
#include "Engine/Renderer/SpotLight.h"
#include "Engine/Input/GamepadState.h"

using namespace DirectX;

class TestScene : public SE::Engine
{
public:
    ~TestScene()
    {
        CancelSceneLoad();
    }

    void Shutdown()
    {
        // The worker may still be creating D3D resources. Join it before the
        // renderer and device are released by the base engine shutdown.
        CancelSceneLoad();
        SE::Engine::Shutdown();
    }

    bool Setup(const std::string& scenePath = "")
    {
        ID3D11Device* device = GetRenderer().GetDevice();

        if (!m_skybox.Init(device, GetRenderer().GetStateCache(), GetShaders())) return false;
        if (!m_environmentLighting.Init(device, GetShaders())) return false;
        if (!m_lights.Init(device))                return false;
        if (!m_pipeline.Init(device, GetAssets(), GetShaders())) return false;
        if (!m_shadowMap.Init(device, GetShaders(), 2048)) return false;

        // Forward HDR render target with readable depth for screen-space effects.
        if (!m_forwardHDR_RT.Init(device, GetWindow().GetWidth(), GetWindow().GetHeight(),
                DXGI_FORMAT_R16G16B16A16_FLOAT, /*withDepth=*/true, /*depthReadable=*/true))
            return false;

        // Normal + roughness MRT target (view-space normal xyz + roughness w)
        if (!m_normalRT.Init(device, GetWindow().GetWidth(), GetWindow().GetHeight(),
                DXGI_FORMAT_R16G16B16A16_FLOAT))
            return false;

        // Tone mapping
        if (!m_toneMap.Init(device, GetShaders())) return false;

        // Point shadow maps — 2 slots, 256x256 per cube face
        for (int i = 0; i < 2; ++i)
            if (!m_pointShadowMaps[i].Init(device, GetShaders(), 256)) return false;

        // Bloom
        if (!m_bloom.Init(device, GetShaders(),
                GetWindow().GetWidth(), GetWindow().GetHeight())) return false;

        // Screen-Space Ambient Occlusion
        if (!m_ssao.Init(device, GetShaders(),
                GetWindow().GetWidth(), GetWindow().GetHeight())) return false;

        // Volumetric fog / god rays
        if (!m_volFog.Init(device, GetShaders(),
                GetWindow().GetWidth(), GetWindow().GetHeight())) return false;

        // FXAA
        if (!m_fxaa.Init(device, GetShaders())) return false;

        // LDR intermediate RT for FXAA input
        if (!m_ldrRT.Init(device, GetWindow().GetWidth(), GetWindow().GetHeight(),
                DXGI_FORMAT_R8G8B8A8_UNORM))
            return false;

        // Spot light
        if (!m_spotLight.Init(device, GetShaders(), 1024)) return false;

        // Scan available scenes
        m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
        RefreshParticlePresets();
        if (m_sceneFiles.empty())
        {
            SE_LOG_ERROR("No scene files found in Assets/Scenes/");
            return false;
        }

        // Determine which scene to load
        std::string targetScene = scenePath;
        if (targetScene.empty() && !m_sceneFiles.empty())
            targetScene = m_sceneFiles[0];

        // Queue the first load so the render loop can present loading feedback
        // before potentially expensive asset import begins.
        m_initialScenePath = targetScene;
        m_pendingSceneLoad = targetScene;
        m_initialSceneLoad = true;
        SE_LOG_INFO("TestScene initial scene queued — %s", targetScene.c_str());
        return true;
    }

    bool ApplyScene(const std::string& scenePath, SE::PreparedLevel&& prepared)
    {
        SE::LevelAsset level = std::move(prepared.level);
        SE::SceneDescriptor desc = std::move(prepared.descriptor);

        auto pumpLoadMessages = [this]() { return GetWindow().PumpMessages(); };
        if (!pumpLoadMessages())
            return false;

        ID3D11Device* device = GetRenderer().GetDevice();

        // --- Mesh entities ---
        // Keep the active level alive until every replacement mesh has imported successfully.
        std::vector<LoadedMesh> newLoadedMeshes;
        newLoadedMeshes.reserve(desc.meshes.size());
        for (size_t meshIndex = 0; meshIndex < desc.meshes.size(); ++meshIndex)
        {
            const auto& meshDesc = desc.meshes[meshIndex];
            if (meshDesc.path.empty())
                continue;

            LoadedMesh loaded;
            loaded.def = meshDesc;
            loaded.mesh = meshIndex < prepared.meshes.size()
                ? std::move(prepared.meshes[meshIndex]) : nullptr;
            if (!loaded.mesh)
            {
                m_sceneLoadError = "Mesh import failed or was cancelled";
                SE_LOG_WARN("Failed to load mesh: %s", meshDesc.path.c_str());
                return false;
            }

            loaded.materials = meshIndex < prepared.materials.size()
                ? std::move(prepared.materials[meshIndex])
                : std::vector<SE::ForwardPipeline::SubMat>{};
            if (!pumpLoadMessages())
                return false;

            if (meshDesc.alphaCutoff > 0.0f)
            {
                for (auto& sm : loaded.materials)
                {
                    if (sm.alphaMode == SE::AlphaMode::Opaque && sm.albedo && sm.albedo->HasAlpha())
                    {
                        sm.alphaMode   = SE::AlphaMode::Cutout;
                        sm.alphaCutoff = meshDesc.alphaCutoff;
                    }
                }
            }
            newLoadedMeshes.push_back(std::move(loaded));
        }

        // --- Runtime world (transactional rebuild) ---
        SE::LevelWorld newWorld;
        if (!newWorld.Instantiate(level))
        {
            m_sceneLoadError = "Could not instantiate level world";
            return false;
        }
        SE::CameraComponent* newCamera = newWorld.GetPrimaryCamera();
        if (!newCamera)
        {
            SE::Entity* cameraEntity = newWorld.GetScene().CreateEntity("Fallback Camera");
            newCamera = cameraEntity->AddComponent<SE::CameraComponent>();
            newCamera->nearZ = desc.camera.nearZ;
            newCamera->farZ = desc.camera.farZ;
        }
        SE::CameraController newCameraController = m_camCtrl;
        newCameraController.freeFly.eye      = { desc.camera.eye[0], desc.camera.eye[1], desc.camera.eye[2] };
        newCameraController.freeFly.yawDeg   = desc.camera.yaw;
        newCameraController.freeFly.pitchDeg = desc.camera.pitch;
        newCameraController.Update(0.0f, GetInput(), *newCamera, GetWindow().GetHandle());

        for (auto& loaded : newLoadedMeshes)
        {
            SE::Entity* meshEntity = newWorld.FindBySourceID(loaded.def.sourceEntityID);
            if (!meshEntity || !(loaded.transform = meshEntity->GetComponent<SE::TransformComponent>()))
            {
                m_sceneLoadError = "Mesh entity was not instantiated";
                return false;
            }
        }

        // Physics ball entity
        SE::Entity* ballEnt = newWorld.GetScene().CreateEntity("PhysBall");
        auto* newBallTransform = ballEnt->AddComponent<SE::TransformComponent>();
        auto* newBallRigidBody = ballEnt->AddComponent<SE::RigidBodyComponent>();
        const float newBallRadius = desc.physics.ballRadius;
        const XMFLOAT3 newBallSpawn = {
            desc.physics.ballSpawn[0], desc.physics.ballSpawn[1], desc.physics.ballSpawn[2]
        };
        newBallTransform->scale = { newBallRadius, newBallRadius, newBallRadius };
        newBallTransform->position = newBallSpawn;
        newBallRigidBody->velocity = { 0.0f, 0.0f, 0.0f };

        // --- Physics world (rebuild) ---
        SE::PhysicsWorld newPhysicsWorld;
        newPhysicsWorld.AddSphere(newBallTransform, newBallRigidBody, newBallRadius);
        const float newFloorY = desc.physics.floor.max[1];
        const SE::OBB newFloor = SE::OBB::FromAABB(
            { desc.physics.floor.min[0], desc.physics.floor.min[1], desc.physics.floor.min[2] },
            { desc.physics.floor.max[0], desc.physics.floor.max[1], desc.physics.floor.max[2] });
        newPhysicsWorld.AddStaticOBB(
            newFloor, desc.physics.floor.friction, desc.physics.floor.restitution);

        // Character controller
        SE::CharacterController newCharacterController;
        newCharacterController.position = {
            desc.physics.characterSpawn[0], desc.physics.characterSpawn[1],
            desc.physics.characterSpawn[2]
        };
        newCharacterController.gravityEnabled = desc.physics.gravity;
        newBallRigidBody->useGravity = desc.physics.gravity;

        // --- JSON scene objects (spheres / planes with explicit PBR textures) ---
        std::vector<LoadedObject> newSceneObjects;
        newSceneObjects.reserve(desc.objects.size());
        for (size_t objectIndex = 0; objectIndex < desc.objects.size(); ++objectIndex)
        {
            if (!pumpLoadMessages())
                return false;

            LoadedObject lo;
            lo.def = desc.objects[objectIndex];
            if (objectIndex < prepared.objectMaterials.size())
                lo.mat = std::move(prepared.objectMaterials[objectIndex]);
            newSceneObjects.push_back(std::move(lo));
        }

        // --- Particle emitters (from scene JSON) ---
        std::vector<std::unique_ptr<SE::ParticleSystem>> newParticleSystems;
        std::vector<SE::SceneDescriptor::ParticleEmitterDesc> newParticleEmitterDescs;
        newParticleSystems.reserve(desc.particles.size());
        newParticleEmitterDescs.reserve(desc.particles.size());
        for (size_t particleIndex = 0; particleIndex < desc.particles.size(); ++particleIndex)
        {
            auto& pe = desc.particles[particleIndex];
            if (!pumpLoadMessages())
                return false;

            auto ps = std::make_unique<SE::ParticleSystem>();
            ps->config.emitRate     = pe.emitRate;
            ps->config.maxParticles = pe.maxParticles;
            ps->config.lifetimeMin  = pe.lifetimeMin;
            ps->config.lifetimeMax  = pe.lifetimeMax;
            ps->config.velocityMin  = { pe.velocityMin[0], pe.velocityMin[1], pe.velocityMin[2] };
            ps->config.velocityMax  = { pe.velocityMax[0], pe.velocityMax[1], pe.velocityMax[2] };
            ps->config.sizeStart    = pe.sizeStart;
            ps->config.sizeEnd      = pe.sizeEnd;
            ps->config.colorStart   = { pe.colorStart[0], pe.colorStart[1], pe.colorStart[2], pe.colorStart[3] };
            ps->config.colorEnd     = { pe.colorEnd[0], pe.colorEnd[1], pe.colorEnd[2], pe.colorEnd[3] };
            ps->config.gravity      = { pe.gravity[0], pe.gravity[1], pe.gravity[2] };
            ps->config.spawnRadius  = pe.spawnRadius;
            ps->config.spawnShape   = pe.spawnShape;
            ps->config.spawnBoxExtents = { pe.spawnBoxExtents[0], pe.spawnBoxExtents[1], pe.spawnBoxExtents[2] };
            ps->config.atlasColumns    = pe.atlasColumns;
            ps->config.atlasRows       = pe.atlasRows;
            ps->config.atlasFrameCount = pe.atlasFrameCount;
            ps->config.atlasSpeed      = pe.atlasSpeed;
            ps->config.textureRotationDegrees = pe.textureRotationDegrees;
            ps->config.softDistance    = pe.softDistance;
            ps->config.aspectRatio     = pe.aspectRatio;
            ps->config.blendMode       = pe.blendMode;
            ps->config.drag            = pe.drag;
            ps->config.shaderMode      = pe.shaderMode;
            ps->config.emitMode        = pe.emitMode;
            ps->config.rotationDegrees = pe.rotationDegrees;
            ps->config.randomRotation  = pe.randomRotation;
            ps->config.tracerStretch   = pe.tracerStretch;
            ps->config.lightingInfluence = pe.lightingInfluence;
            ps->config.position        = { pe.position[0], pe.position[1], pe.position[2] };
            if (!ps->Init(device, GetShaders()))
            {
                m_sceneLoadError = "Could not initialize particle emitter";
                return false;
            }
            if (particleIndex < prepared.particleTextures.size())
                ps->SetTexture(prepared.particleTextures[particleIndex]);
            newParticleSystems.push_back(std::move(ps));
            newParticleEmitterDescs.push_back(pe);
        }
        std::vector<int> newSelectedParticlePreset(newParticleSystems.size(), -1);
        for (int i = 0; i < (int)newParticleSystems.size(); ++i)
        {
            for (int p = 0; p < (int)m_particlePresetFiles.size(); ++p)
            {
                SE::SceneDescriptor::ParticleEmitterDesc preset;
                if (!SE::SceneLoader::LoadParticlePreset(m_particlePresetFiles[p], preset))
                    continue;
                if (IsSameParticlePreset(newParticleEmitterDescs[i], preset))
                {
                    newSelectedParticlePreset[i] = p;
                    break;
                }
            }
        }

        // Commit only after every replacement mesh has completed its import.
        m_world            = std::move(newWorld);
        m_camera           = newCamera;
        m_camCtrl          = std::move(newCameraController);
        m_ballTransform    = newBallTransform;
        m_ballRigidBody    = newBallRigidBody;
        m_ballRadius       = newBallRadius;
        m_ballSpawn        = newBallSpawn;
        m_physicsWorld     = std::move(newPhysicsWorld);
        m_floorY           = newFloorY;
        m_obbFloor         = newFloor;
        m_cc               = std::move(newCharacterController);
        m_gravityEnabled   = desc.physics.gravity;
        m_sceneObjects     = std::move(newSceneObjects);
        m_particleSystems  = std::move(newParticleSystems);
        m_particleEmitterDescs = std::move(newParticleEmitterDescs);
        m_selectedParticlePreset = std::move(newSelectedParticlePreset);
        m_loadedMeshes     = std::move(newLoadedMeshes);
        m_environmentTexture = std::move(prepared.environment);
        m_levelDiagnostics = std::move(prepared.diagnostics);
        m_skybox.SetPanorama(m_environmentTexture ? m_environmentTexture->GetSRV() : nullptr);
        if (!m_environmentLighting.Build(device, GetRenderer().GetContext(),
                                         m_environmentTexture ? m_environmentTexture->GetSRV() : nullptr))
            SE_LOG_WARN("Environment lighting generation failed; using neutral fallback");

        m_planarObjectIndex = -1;
        m_planarReflectionScale = 0.5f;
        for (size_t objectIndex = 0; objectIndex < m_sceneObjects.size(); ++objectIndex)
        {
            const auto& object = m_sceneObjects[objectIndex].def;
            if (object.type == SE::SceneDescriptor::SceneObject::Type::Plane && object.planarReflection)
            {
                m_planarObjectIndex = static_cast<int>(objectIndex);
                m_planarReflectionY = object.position[1];
                m_planarReflectionStrength = object.reflectionStrength;
                m_planarReflectionScale = (std::max)(0.25f, (std::min)(1.0f, object.reflectionResolutionScale));
                break;
            }
        }
        m_planarReflectionRT.Shutdown();
        if (m_planarObjectIndex >= 0)
        {
            const uint32_t reflectionWidth = (std::max)(1u, static_cast<uint32_t>(GetWindow().GetWidth() * m_planarReflectionScale));
            const uint32_t reflectionHeight = (std::max)(1u, static_cast<uint32_t>(GetWindow().GetHeight() * m_planarReflectionScale));
            if (!m_planarReflectionRT.Init(device, reflectionWidth, reflectionHeight,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, true))
            {
                SE_LOG_WARN("Planar reflection target creation failed; using environment reflection");
                m_planarObjectIndex = -1;
            }
        }
        m_lights.elevDeg = desc.sun.elevation;
        m_lights.azimDeg = desc.sun.azimuth;
        m_lights.lightIntensity = desc.sun.intensity;
        m_lights.lightColor[0] = desc.sun.color[0];
        m_lights.lightColor[1] = desc.sun.color[1];
        m_lights.lightColor[2] = desc.sun.color[2];
        m_lights.iblIntensity = desc.iblIntensity;
        m_lights.diffuseIBLSaturation = desc.diffuseIBLSaturation;
        m_lights.numLights = (std::min)(static_cast<int>(desc.pointLights.size()), 8);
        m_pointLightDescs = desc.pointLights;
        std::fill(std::begin(m_lightCastsShadow), std::end(m_lightCastsShadow), false);
        for (int i = 0; i < m_lights.numLights; ++i)
        {
            const auto& light = desc.pointLights[i];
            m_lights.lights[i].position = { light.position[0], light.position[1], light.position[2] };
            m_lights.lights[i].color = { light.color[0], light.color[1], light.color[2] };
            m_lights.lights[i].radius = light.radius;
            m_lightCastsShadow[i] = light.castShadow;
        }
        m_toneMap.op = static_cast<SE::ToneMap::Operator>(desc.toneMapping.op);
        m_toneMap.exposure = desc.toneMapping.exposure;
        m_toneMap.gammaCorrect = desc.toneMapping.gammaCorrect;
        m_bloom.enabled = desc.bloom.enabled;
        m_bloom.threshold = desc.bloom.threshold;
        m_bloom.intensity = desc.bloom.intensity;
        m_bloom.scatter = desc.bloom.scatter;
        m_volFog.enabled = desc.fog.enabled;
        m_volFog.density = desc.fog.density;
        m_volFog.anisotropy = desc.fog.anisotropy;
        m_volFog.scatterIntensity = desc.fog.scatterIntensity;
        m_volFog.steps = desc.fog.steps;
        m_volFog.maxDistance = desc.fog.maxDistance;
        std::copy(std::begin(desc.fog.ambientColor), std::end(desc.fog.ambientColor), m_volFog.ambientColor);
        m_volFog.ambientIntensity = desc.fog.ambientIntensity;
        m_volFog.heightFalloff = desc.fog.heightFalloff;
        m_volFog.heightBase = desc.fog.heightBase;
        m_volFog.noiseStrength = desc.fog.noiseStrength;
        m_volFog.noiseScale = desc.fog.noiseScale;
        m_volFog.noiseSpeed = desc.fog.noiseSpeed;
        m_volFog.nearClearDistance = desc.fog.nearClearDistance;
        m_volFog.distanceDensity = desc.fog.distanceDensity;
        m_currentScenePath = scenePath;
        m_currentDesc      = desc;
        m_sceneLoadError.clear();
        for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
            if (m_sceneFiles[i] == scenePath) { m_selectedScene = i; break; }

        // Update window title
        std::wstring title = L"FoxEngine " + std::wstring(desc.name.begin(), desc.name.end());
        SetWindowTextW(GetWindow().GetHandle(), title.c_str());

        return true;
    }

    static void ApplyParticleDescToSystem(const SE::SceneDescriptor::ParticleEmitterDesc& d, SE::ParticleSystem& ps)
    {
        ps.config.position = { d.position[0], d.position[1], d.position[2] };
        ps.config.emitRate = d.emitRate;
        ps.config.maxParticles = d.maxParticles;
        ps.config.lifetimeMin = d.lifetimeMin;
        ps.config.lifetimeMax = d.lifetimeMax;
        ps.config.velocityMin = { d.velocityMin[0], d.velocityMin[1], d.velocityMin[2] };
        ps.config.velocityMax = { d.velocityMax[0], d.velocityMax[1], d.velocityMax[2] };
        ps.config.sizeStart = d.sizeStart;
        ps.config.sizeEnd = d.sizeEnd;
        ps.config.colorStart = { d.colorStart[0], d.colorStart[1], d.colorStart[2], d.colorStart[3] };
        ps.config.colorEnd = { d.colorEnd[0], d.colorEnd[1], d.colorEnd[2], d.colorEnd[3] };
        ps.config.gravity = { d.gravity[0], d.gravity[1], d.gravity[2] };
        ps.config.spawnRadius = d.spawnRadius;
        ps.config.spawnShape = d.spawnShape;
        ps.config.spawnBoxExtents = { d.spawnBoxExtents[0], d.spawnBoxExtents[1], d.spawnBoxExtents[2] };
        ps.config.atlasColumns = d.atlasColumns;
        ps.config.atlasRows = d.atlasRows;
        ps.config.atlasFrameCount = d.atlasFrameCount;
        ps.config.atlasSpeed = d.atlasSpeed;
        ps.config.textureRotationDegrees = d.textureRotationDegrees;
        ps.config.softDistance = d.softDistance;
        ps.config.aspectRatio = d.aspectRatio;
        ps.config.blendMode = d.blendMode;
        ps.config.drag = d.drag;
        ps.config.shaderMode = d.shaderMode;
        ps.config.emitMode = d.emitMode;
        ps.config.rotationDegrees = d.rotationDegrees;
        ps.config.randomRotation = d.randomRotation;
        ps.config.tracerStretch = d.tracerStretch;
        ps.config.lightingInfluence = d.lightingInfluence;
    }

    static bool NearlyEqual(float a, float b, float eps = 0.0001f)
    {
        return std::fabs(a - b) <= eps;
    }

    static bool EqualFloat3(const std::array<float, 3>& a, const std::array<float, 3>& b, float eps = 0.0001f)
    {
        return NearlyEqual(a[0], b[0], eps) && NearlyEqual(a[1], b[1], eps) && NearlyEqual(a[2], b[2], eps);
    }

    static bool EqualFloat4(const std::array<float, 4>& a, const std::array<float, 4>& b, float eps = 0.0001f)
    {
        return NearlyEqual(a[0], b[0], eps) && NearlyEqual(a[1], b[1], eps) &&
               NearlyEqual(a[2], b[2], eps) && NearlyEqual(a[3], b[3], eps);
    }

    static bool IsSameParticlePreset(const SE::SceneDescriptor::ParticleEmitterDesc& a,
                                     const SE::SceneDescriptor::ParticleEmitterDesc& b)
    {
        // Position is scene placement, not part of the preset's look — excluded from comparison.
        return NearlyEqual(a.emitRate, b.emitRate) &&
               a.maxParticles == b.maxParticles &&
               NearlyEqual(a.lifetimeMin, b.lifetimeMin) &&
               NearlyEqual(a.lifetimeMax, b.lifetimeMax) &&
               EqualFloat3(a.velocityMin, b.velocityMin) &&
               EqualFloat3(a.velocityMax, b.velocityMax) &&
               NearlyEqual(a.sizeStart, b.sizeStart) &&
               NearlyEqual(a.sizeEnd, b.sizeEnd) &&
               EqualFloat4(a.colorStart, b.colorStart) &&
               EqualFloat4(a.colorEnd, b.colorEnd) &&
               EqualFloat3(a.gravity, b.gravity) &&
               NearlyEqual(a.spawnRadius, b.spawnRadius) &&
               a.spawnShape == b.spawnShape &&
               EqualFloat3(a.spawnBoxExtents, b.spawnBoxExtents) &&
               a.texture == b.texture &&
               a.atlasColumns == b.atlasColumns &&
               a.atlasRows == b.atlasRows &&
               a.atlasFrameCount == b.atlasFrameCount &&
               NearlyEqual(a.atlasSpeed, b.atlasSpeed) &&
               NearlyEqual(a.textureRotationDegrees, b.textureRotationDegrees) &&
               NearlyEqual(a.softDistance, b.softDistance) &&
               NearlyEqual(a.aspectRatio, b.aspectRatio) &&
               a.blendMode == b.blendMode &&
               NearlyEqual(a.drag, b.drag) &&
               a.shaderMode == b.shaderMode &&
               a.emitMode == b.emitMode &&
               NearlyEqual(a.rotationDegrees, b.rotationDegrees) &&
               a.randomRotation == b.randomRotation &&
               NearlyEqual(a.tracerStretch, b.tracerStretch) &&
               NearlyEqual(a.lightingInfluence, b.lightingInfluence);
    }

    void RefreshParticlePresets()
    {
        m_particlePresetFiles = SE::SceneLoader::ScanParticlePresetDirectory("Assets/Particles/Presets");
    }

    void ResetBall()
    {
        m_ballTransform->position = m_ballSpawn;
        m_ballRigidBody->velocity = { 0.0f, 0.0f, 0.0f };
    }

protected:
    void OnUpdate() override
    {
        if (m_completeSceneLoadNextFrame)
        {
            m_completeSceneLoadNextFrame = false;
            m_initialSceneLoad = false;
            m_loadingScenePath.clear();
            SE_LOG_INFO("TestScene ready — %s", m_currentDesc.name.c_str());
        }

        bool startedLoadThisFrame = false;
        if (m_loadingScenePath.empty() && !m_pendingSceneLoad.empty())
        {
            const std::string scenePath = std::move(m_pendingSceneLoad);
            m_pendingSceneLoad.clear();
            BeginAsyncSceneLoad(scenePath);
            startedLoadThisFrame = true;
        }

        if (!startedLoadThisFrame && !m_loadingScenePath.empty())
        {
            m_levelLoad.PumpUploads(GetRenderer().GetDevice(), GetRenderer().GetContext(),
                                    GetAssets());
            const SE::LevelLoadStatus operationStatus = m_levelLoad.GetStatus();
            if (operationStatus.state == SE::LevelLoadState::Failed ||
                operationStatus.state == SE::LevelLoadState::Cancelled)
            {
                m_sceneLoadError = operationStatus.failureReason.empty()
                    ? "LEVEL LOAD FAILED" : operationStatus.failureReason;
                m_loadingScenePath.clear();
            }
            else if (operationStatus.state == SE::LevelLoadState::Complete)
            {
                auto prepared = m_levelLoad.TakePreparedLevel();
                if (prepared)
                {
                    const bool loaded = ApplyScene(m_loadingScenePath, std::move(*prepared));
                    if (!loaded)
                    {
                        m_sceneLoadError = "LEVEL LOAD FAILED";
                    }
                    else
                    {
                        m_completeSceneLoadNextFrame = true;
                    }
                }
                m_loadingScenePath.clear();
            }
        }

        if (m_initialSceneLoad)
        {
            if (!m_sceneLoadError.empty())
            {
                if (GetInput().IsKeyPressed('R'))
                {
                    m_sceneLoadError.clear();
                    m_pendingSceneLoad = m_initialScenePath;
                }
                if (GetInput().IsKeyPressed(VK_ESCAPE))
                    PostMessageW(GetWindow().GetHandle(), WM_CLOSE, 0, 0);
            }
            DrawLoadingUI();
            return;
        }
        if (!m_loadingScenePath.empty())
        {
            DrawLoadingUI();
            return;
        }

        ID3D11DeviceContext* ctx = GetRenderer().GetContext();
        float dt     = GetClock().GetDeltaTime();
        float aspect = (float)GetWindow().GetWidth() / (float)GetWindow().GetHeight();

        bool          mouseBlocked = ImGui::GetIO().WantCaptureMouse;
        SE::CameraController::Mode prevMode = m_camCtrl.GetMode();

        m_camCtrl.Update(dt, GetInput(), *m_camera,
                         GetWindow().GetHandle(), mouseBlocked);

        SE::CameraController::Mode mode = m_camCtrl.GetMode();

        // Init character position when first entering FPS mode.
        if (prevMode == SE::CameraController::Mode::FreeFly &&
            mode     == SE::CameraController::Mode::FPS)
        {
            m_cc.position = { m_camera->eye.x,
                              m_camera->eye.y - m_cc.eyeHeight,
                              m_camera->eye.z };
            m_cc.velY = 0.0f;
        }

        // Drive character + camera in FPS mode.
        if (mode == SE::CameraController::Mode::FPS)
        {
            auto& inp = GetInput();
            float yawRad = XMConvertToRadians(m_camCtrl.fps.yawDeg);
            XMFLOAT3 fwd   = { sinf(yawRad), 0.0f,  cosf(yawRad) };
            XMFLOAT3 right = { cosf(yawRad), 0.0f, -sinf(yawRad) };

            XMFLOAT3 wishVel = { 0.0f, 0.0f, 0.0f };
            if (inp.IsKeyDown('W')) { wishVel.x += fwd.x;   wishVel.z += fwd.z; }
            if (inp.IsKeyDown('S')) { wishVel.x -= fwd.x;   wishVel.z -= fwd.z; }
            if (inp.IsKeyDown('D')) { wishVel.x += right.x; wishVel.z += right.z; }
            if (inp.IsKeyDown('A')) { wishVel.x -= right.x; wishVel.z -= right.z; }

            // Normalise diagonal, then scale to moveSpeed.
            float len2 = wishVel.x * wishVel.x + wishVel.z * wishVel.z;
            if (len2 > 1.0f) {
                float inv = 1.0f / sqrtf(len2);
                wishVel.x *= inv; wishVel.z *= inv;
            }
            wishVel.x *= m_cc.moveSpeed;
            wishVel.z *= m_cc.moveSpeed;

            if (inp.IsKeyPressed(VK_SPACE)) m_cc.Jump();

            m_physicsWorld.StepCharacter(m_cc, wishVel, dt);

            // Sync camera to character eye.
            float pitchRad = XMConvertToRadians(m_camCtrl.fps.pitchDeg);
            XMFLOAT3 eye  = m_cc.GetEyePosition();
            XMFLOAT3 look = {
                sinf(yawRad) * cosf(pitchRad),
                sinf(pitchRad),
                cosf(yawRad) * cosf(pitchRad)
            };
            m_camera->eye    = eye;
            m_camera->target = { eye.x + look.x, eye.y + look.y, eye.z + look.z };
            m_camera->up     = { 0.0f, 1.0f, 0.0f };
        }

        m_world.GetScene().Update(dt);
        m_physicsWorld.Step(dt);
        for (auto& ps : m_particleSystems)
            ps->Update(GetRenderer().GetContext(), dt);

        UpdatePointLightFlicker(static_cast<float>(GetClock().GetTotalTime()));

        XMMATRIX view = m_camera->GetViewMatrix();
        XMMATRIX proj = m_camera->GetProjectionMatrix(aspect);
        m_cachedProj = proj;
        m_cachedView = view;  // Store for volumetric fog in OnPostProcess

        DrawUI(view, proj);

        // Cascaded shadow pass
        {
            float er = XMConvertToRadians(m_lights.elevDeg);
            float ar = XMConvertToRadians(m_lights.azimDeg);
            XMFLOAT3 lightDir = { cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar) };
            m_shadowMap.Update(lightDir, view, proj, m_camera->nearZ, m_camera->farZ);
            for (int c = 0; c < SE::CSM_NUM_CASCADES; ++c)
            {
                m_shadowMap.BeginCascade(ctx, c);
                for (const auto& loaded : m_loadedMeshes)
                    if (loaded.def.castShadows && loaded.mesh && loaded.transform)
                        m_shadowMap.DrawMesh(ctx, *loaded.mesh, loaded.transform->GetWorldMatrix());
                m_shadowMap.DrawSphere(ctx, m_ballTransform->position, m_ballRadius);
                m_shadowMap.EndCascade(ctx);
            }
        }

        // Point shadow passes
        {
            int numCasters = 0;
            for (int i = 0; i < 2 && i < m_lights.numLights; ++i)
                if (m_lightCastsShadow[i]) numCasters = i + 1; else break;
            for (int li = 0; li < numCasters; ++li)
            {
                auto& light = m_lights.lights[li];
                for (int face = 0; face < 6; ++face)
                {
                    m_pointShadowMaps[li].BeginFace(ctx, face, light.position, light.radius);
                    for (const auto& loaded : m_loadedMeshes)
                        if (loaded.def.castShadows && loaded.mesh && loaded.transform)
                            m_pointShadowMaps[li].DrawMesh(
                                ctx, *loaded.mesh, loaded.transform->GetWorldMatrix());
                    m_pointShadowMaps[li].EndFace(ctx);
                }
            }
        }

        // Spot light shadow pass
        if (m_spotLight.enabled)
        {
            m_spotLight.BeginShadowPass(ctx);
            for (const auto& loaded : m_loadedMeshes)
                if (loaded.def.castShadows && loaded.mesh && loaded.transform)
                    m_spotLight.DrawMesh(ctx, *loaded.mesh, loaded.transform->GetWorldMatrix());
            m_spotLight.EndShadowPass(ctx);
        }

        ID3D11ShaderResourceView* fallbackEnvironment = GetAssets().GetDefaultBlack()->GetSRV();
        ID3D11ShaderResourceView* irradianceSRV = m_environmentLighting.GetIrradianceSRV();
        ID3D11ShaderResourceView* specularSRV = m_environmentLighting.GetSpecularSRV();
        ID3D11ShaderResourceView* brdfLutSRV = m_environmentLighting.GetBRDFLutSRV();
        if (!irradianceSRV) irradianceSRV = fallbackEnvironment;
        if (!specularSRV) specularSRV = fallbackEnvironment;
        if (!brdfLutSRV) brdfLutSRV = fallbackEnvironment;

        // One bounded planar reflection pass for authored horizontal reflectors.
        if (m_planarObjectIndex >= 0 && m_planarReflectionRT.GetRTV())
        {
            XMFLOAT3 reflectedEye = m_camera->eye;
            XMFLOAT3 reflectedTarget = m_camera->target;
            XMFLOAT3 reflectedUp = m_camera->up;
            reflectedEye.y = 2.0f * m_planarReflectionY - reflectedEye.y;
            reflectedTarget.y = 2.0f * m_planarReflectionY - reflectedTarget.y;
            reflectedUp.y = -reflectedUp.y;
            XMMATRIX reflectedView = XMMatrixLookAtLH(
                XMLoadFloat3(&reflectedEye), XMLoadFloat3(&reflectedTarget), XMLoadFloat3(&reflectedUp));
            m_planarReflectionViewProj = reflectedView * proj;

            const float reflectionClear[4] = { 0, 0, 0, 1 };
            m_planarReflectionRT.Begin(ctx, reflectionClear);
            m_skybox.Draw(ctx, reflectedView, proj);
            m_shadowMap.BindForLitPass(ctx);
            m_lights.BindPS(ctx, reflectedEye, m_shadowMap.GetLightViewProj());
            m_pipeline.Begin(ctx, reflectedView, proj);
            m_pipeline.BindEnvironment(ctx, irradianceSRV, specularSRV, brdfLutSRV);

            int reflectedShadowCount = 0;
            for (int i = 0; i < 2 && i < m_lights.numLights; ++i)
                if (m_lightCastsShadow[i]) reflectedShadowCount = i + 1; else break;
            ID3D11ShaderResourceView* reflectedShadowSRVs[2] = { nullptr, nullptr };
            for (int i = 0; i < reflectedShadowCount; ++i)
                reflectedShadowSRVs[i] = m_pointShadowMaps[i].GetSRV();
            m_pipeline.BindPointShadows(ctx, reflectedShadowSRVs[0], reflectedShadowSRVs[1],
                                        reflectedShadowCount, m_pointShadowBias);
            m_spotLight.BindForLitPass(ctx);
            m_pipeline.SetReflectionPass(ctx, true, m_planarReflectionY);

            for (const auto& loaded : m_loadedMeshes)
                if (loaded.mesh && loaded.transform)
                    m_pipeline.SubmitMesh(*loaded.mesh, loaded.transform->GetWorldMatrix(), loaded.materials);
            if (!m_loadedMeshes.empty()) m_pipeline.Flush(ctx);
            m_pipeline.DrawSphere(ctx, m_ballTransform->position, m_ballRadius, { 1.0f, 0.45f, 0.05f });
            for (size_t objectIndex = 0; objectIndex < m_sceneObjects.size(); ++objectIndex)
            {
                if (static_cast<int>(objectIndex) == m_planarObjectIndex) continue;
                auto& object = m_sceneObjects[objectIndex];
                XMFLOAT3 pos = { object.def.position[0], object.def.position[1], object.def.position[2] };
                XMFLOAT3 tint = { object.def.tint[0], object.def.tint[1], object.def.tint[2] };
                XMFLOAT3 emissive = { object.def.emissiveColor[0], object.def.emissiveColor[1], object.def.emissiveColor[2] };
                if (object.def.type == SE::SceneDescriptor::SceneObject::Type::Sphere)
                    m_pipeline.DrawPBRSphere(ctx, pos, object.def.radius, object.mat,
                                             object.def.metallicScale, object.def.roughnessScale,
                                             tint, object.def.emissiveIntensity, emissive);
                else
                    m_pipeline.DrawPBRPlane(ctx, pos, object.def.halfSizeX, object.def.halfSizeZ,
                                            object.mat, object.def.metallicScale,
                                            object.def.roughnessScale, tint,
                                            object.def.emissiveIntensity, emissive);
            }
            m_pipeline.SetReflectionPass(ctx, false);
            m_shadowMap.Unbind(ctx);
            ID3D11ShaderResourceView* nullPointShadows[2] = { nullptr, nullptr };
            ctx->PSSetShaderResources(5, 2, nullPointShadows);
            m_planarReflectionRT.End(ctx);
            m_planarReflectionRT.GenerateMips(ctx);
        }

        // --- Forward path (HDR + PBR + IBL) ---
        {
            ID3D11RenderTargetView* rtvs[2] = { m_forwardHDR_RT.GetRTV(), m_normalRT.GetRTV() };
            ID3D11DepthStencilView* dsv = m_forwardHDR_RT.GetDSV();
            const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            const float clearNorm[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            ctx->OMSetRenderTargets(2, rtvs, dsv);
            ctx->ClearRenderTargetView(rtvs[0], black);
            ctx->ClearRenderTargetView(rtvs[1], clearNorm);
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            D3D11_VIEWPORT vp = {};
            vp.Width = (float)m_forwardHDR_RT.GetWidth();
            vp.Height = (float)m_forwardHDR_RT.GetHeight();
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
        }

        m_skybox.Draw(ctx, view, proj);

        // Collect point shadow SRVs
        int numPtShadows = 0;
        for (int i = 0; i < 2 && i < m_lights.numLights; ++i)
            if (m_lightCastsShadow[i]) numPtShadows = i + 1; else break;
        ID3D11ShaderResourceView* ptSRVs[2] = { nullptr, nullptr };
        for (int i = 0; i < numPtShadows; ++i)
            ptSRVs[i] = m_pointShadowMaps[i].GetSRV();

        m_shadowMap.BindForLitPass(ctx);
        m_lights.BindPS(ctx, m_camera->eye, m_shadowMap.GetLightViewProj());
        m_pipeline.Begin(ctx, view, proj);
        m_pipeline.BindEnvironment(ctx, irradianceSRV, specularSRV, brdfLutSRV);
        m_pipeline.BindPointShadows(ctx, ptSRVs[0], ptSRVs[1], numPtShadows, m_pointShadowBias);
        m_spotLight.BindForLitPass(ctx);
        m_pipeline.SetGlobalMaterialParams(
            { m_matTint[0], m_matTint[1], m_matTint[2] }, m_roughnessScale, m_metallic,
            m_debugShadow ? 1.0f : 0.0f);
        for (const auto& loaded : m_loadedMeshes)
            if (loaded.mesh && loaded.transform)
                m_pipeline.SubmitMesh(
                    *loaded.mesh, loaded.transform->GetWorldMatrix(), loaded.materials);
        if (!m_loadedMeshes.empty())
            m_pipeline.Flush(ctx);
        m_pipeline.DrawSphere(ctx, m_ballTransform->position, m_ballRadius, { 1.0f, 0.45f, 0.05f });

        // JSON-driven scene objects — texture maps drive all PBR values (scalars = 1.0)
        for (size_t objectIndex = 0; objectIndex < m_sceneObjects.size(); ++objectIndex)
        {
            auto& lo = m_sceneObjects[objectIndex];
            using Type = SE::SceneDescriptor::SceneObject::Type;
            XMFLOAT3 pos  = { lo.def.position[0], lo.def.position[1], lo.def.position[2] };
            XMFLOAT3 tint = { lo.def.tint[0],     lo.def.tint[1],     lo.def.tint[2] };
            XMFLOAT3 eCol = { lo.def.emissiveColor[0], lo.def.emissiveColor[1], lo.def.emissiveColor[2] };
            if (lo.def.type == Type::Sphere)
                m_pipeline.DrawPBRSphere(ctx, pos, lo.def.radius, lo.mat,
                                         lo.def.metallicScale, lo.def.roughnessScale,
                                         tint, lo.def.emissiveIntensity, eCol);
            else
            {
                if (static_cast<int>(objectIndex) == m_planarObjectIndex)
                    m_pipeline.BindPlanarReflection(ctx, m_planarReflectionRT.GetSRV(),
                                                    m_planarReflectionViewProj,
                                                    m_planarReflectionY,
                                                    m_planarReflectionStrength);
                m_pipeline.DrawPBRPlane(ctx, pos, lo.def.halfSizeX, lo.def.halfSizeZ,
                                        lo.mat, lo.def.metallicScale, lo.def.roughnessScale,
                                        tint, lo.def.emissiveIntensity, eCol);
                if (static_cast<int>(objectIndex) == m_planarObjectIndex)
                    m_pipeline.ClearPlanarReflection(ctx);
            }
        }

        m_shadowMap.Unbind(ctx);

        // Unbind point shadow SRVs
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(5, 2, nullSRVs);

        if (m_castRay)
        {
            SE::Ray ray;
            XMVECTOR eye = XMLoadFloat3(&m_camera->eye);
            XMVECTOR dir = XMVector3Normalize(
                XMVectorSubtract(XMLoadFloat3(&m_camera->target), eye));
            XMStoreFloat3(&ray.origin,    eye);
            XMStoreFloat3(&ray.direction, dir);
            m_rayHitValid = m_physicsWorld.Raycast(ray, m_rayHit);
        }
        else
        {
            m_rayHitValid = false;
        }

        if (m_showColliders)
        {
            m_pipeline.DrawWireSphere(ctx, m_ballTransform->position, m_ballRadius,
                                      { 0.0f, 1.0f, 0.2f });
            m_pipeline.DrawWireBox(ctx, m_obbFloor.GetWorldMatrix(), { 1.0f, 0.85f, 0.1f });

            XMFLOAT3 bottom = { m_cc.position.x, m_cc.position.y + m_cc.radius, m_cc.position.z };
            m_pipeline.DrawWireSphere(ctx, bottom, m_cc.radius, { 1.0f, 0.4f, 0.8f });
            {
                XMFLOAT3 col = m_cc.isGrounded ? XMFLOAT3{ 1.0f, 0.0f, 1.0f }
                                               : XMFLOAT3{ 0.25f, 0.0f, 0.25f };
                XMFLOAT3 tip = { bottom.x + m_cc.contactNormal.x,
                                 bottom.y + m_cc.contactNormal.y,
                                 bottom.z + m_cc.contactNormal.z };
                m_pipeline.DrawLine(ctx, bottom, tip, col);
            }
        }

        if (m_rayHitValid)
        {
            m_pipeline.DrawWireSphere(ctx, m_rayHit.point, 0.15f, { 1.0f, 1.0f, 1.0f });
            m_pipeline.DrawLine(ctx, m_rayHit.point,
                { m_rayHit.point.x + m_rayHit.normal.x,
                  m_rayHit.point.y + m_rayHit.normal.y,
                  m_rayHit.point.z + m_rayHit.normal.z },
                { 1.0f, 1.0f, 0.0f });
        }

        // Spot light icon (small sphere + direction line)
        if (m_spotLight.enabled)
        {
            m_pipeline.DrawWireSphere(ctx, m_spotLight.position, 0.3f, m_spotLight.color);
            XMFLOAT3 tip = {
                m_spotLight.position.x + m_spotLight.direction.x * 2.0f,
                m_spotLight.position.y + m_spotLight.direction.y * 2.0f,
                m_spotLight.position.z + m_spotLight.direction.z * 2.0f
            };
            m_pipeline.DrawLine(ctx, m_spotLight.position, tip, m_spotLight.color);
        }

        // Particles — render into HDR RT with depth read (no write)
        // Use read-only DSV so depth SRV can be bound simultaneously for soft particles
        if (!m_particleSystems.empty())
        {
            ID3D11RenderTargetView* rtv = m_forwardHDR_RT.GetRTV();
            auto* readOnlyDSV = m_forwardHDR_RT.GetReadOnlyDSV();
            auto* dsv = readOnlyDSV ? readOnlyDSV : m_forwardHDR_RT.GetDSV();
            ctx->OMSetRenderTargets(1, &rtv, dsv);
            for (auto& ps : m_particleSystems)
            {
                const float er = XMConvertToRadians(m_lights.elevDeg);
                const float ar = XMConvertToRadians(m_lights.azimDeg);
                XMFLOAT3 particleLightDir = {
                    cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar)
                };
                XMFLOAT3 particleLightColor = {
                    m_lights.lightColor[0] * m_lights.lightIntensity,
                    m_lights.lightColor[1] * m_lights.lightIntensity,
                    m_lights.lightColor[2] * m_lights.lightIntensity
                };
                ps->Render(ctx, view, proj,
                    m_forwardHDR_RT.GetWidth(), m_forwardHDR_RT.GetHeight(),
                    m_forwardHDR_RT.GetDepthSRV(),
                    m_camera->nearZ, m_camera->farZ,
                    particleLightDir, particleLightColor,
                    0.18f * m_lights.iblIntensity);
            }
        }
    }

    void OnPostProcess() override
    {
        auto* ctx = GetRenderer().GetContext();
        if (m_initialSceneLoad || !m_loadingScenePath.empty())
        {
            // BeginFrame clears the MSAA surface, not the swap chain. Resolve it so
            // the loading UI is drawn over a known background before Present.
            GetRenderer().ResolveToBackBuffer();
            return;
        }

        uint32_t w = GetWindow().GetWidth(), h = GetWindow().GetHeight();

        // Resize render targets when the window changes size.
        if (m_forwardHDR_RT.GetWidth() != w || m_forwardHDR_RT.GetHeight() != h)
        {
            auto* dev = GetRenderer().GetDevice();
            m_forwardHDR_RT.Shutdown();
            m_forwardHDR_RT.Init(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true);
            m_normalRT.Shutdown();
            m_normalRT.Init(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            m_bloom.Resize(dev, w, h);
            m_ssao.Resize(dev, w, h);
            m_volFog.Resize(dev, w, h);
            m_ldrRT.Shutdown();
            m_ldrRT.Init(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (m_planarObjectIndex >= 0)
            {
                m_planarReflectionRT.Shutdown();
                m_planarReflectionRT.Init(dev,
                    (std::max)(1u, static_cast<uint32_t>(w * m_planarReflectionScale)),
                    (std::max)(1u, static_cast<uint32_t>(h * m_planarReflectionScale)),
                    DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, true);
            }
        }

        SE::RenderTarget& hdrRT = m_forwardHDR_RT;

        // Unbind MRT — only need single RTV for post-process
        {
            ID3D11RenderTargetView* nullRTV[2] = { nullptr, nullptr };
            ctx->OMSetRenderTargets(2, nullRTV, nullptr);
        }

        // Screen-Space Ambient Occlusion.
        if (m_ssao.enabled)
            m_ssao.Apply(ctx, hdrRT, hdrRT.GetDepthSRV(), m_normalRT.GetSRV(), m_cachedProj);

        // Volumetric fog / god rays (in-scatter into hdrRT before bloom picks up shafts)
        if (m_volFog.enabled)
        {
            float er = DirectX::XMConvertToRadians(m_lights.elevDeg);
            float ar = DirectX::XMConvertToRadians(m_lights.azimDeg);
            XMFLOAT3 lightDir = { cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar) };
            XMFLOAT3 lightColor = { m_lights.lightColor[0] * m_lights.lightIntensity,
                                    m_lights.lightColor[1] * m_lights.lightIntensity,
                                    m_lights.lightColor[2] * m_lights.lightIntensity };
            XMMATRIX cascadeVP[3] = { m_shadowMap.GetCascadeVP(0),
                                      m_shadowMap.GetCascadeVP(1),
                                      m_shadowMap.GetCascadeVP(2) };
            float splitFar[2] = { m_shadowMap.GetSplitFar(0), m_shadowMap.GetSplitFar(1) };
            XMFLOAT3 spotColor = { m_spotLight.color.x * m_spotLight.intensity,
                                       m_spotLight.color.y * m_spotLight.intensity,
                                       m_spotLight.color.z * m_spotLight.intensity };
            m_volFog.Apply(ctx, hdrRT, hdrRT.GetDepthSRV(),
                           m_shadowMap.GetShadowSRV(), m_shadowMap.GetShadowSampler(),
                           m_skybox.GetPanoramaSRV(), m_spotLight.GetSRV(),
                           m_cachedView, m_cachedProj, m_camera->eye,
                           lightDir, lightColor,
                           m_spotLight.position, m_spotLight.direction, spotColor,
                           m_spotLight.range,
                           cosf(DirectX::XMConvertToRadians(m_spotLight.innerAngle)),
                           cosf(DirectX::XMConvertToRadians(m_spotLight.outerAngle)),
                           m_spotLight.shadowBias, m_spotLight.volumetricIntensity,
                           m_spotLight.enabled ? 1 : 0, m_spotLight.GetViewProj(),
                           cascadeVP, splitFar);
        }

        if (m_bloom.enabled)
            m_bloom.Apply(ctx, hdrRT);

        if (m_fxaa.enabled)
        {
            // Tone map into intermediate LDR RT, then FXAA to backbuffer
            float clearBlack[4] = { 0, 0, 0, 1 };
            m_ldrRT.Begin(ctx, clearBlack);
            m_toneMap.Apply(ctx, hdrRT.GetSRV(), w, h);
            m_ldrRT.End(ctx);

            GetRenderer().BindBackBuffer(ctx);
            m_fxaa.Apply(ctx, m_ldrRT.GetSRV(), w, h);
        }
        else
        {
            GetRenderer().BindBackBuffer(ctx);
            m_toneMap.Apply(ctx, hdrRT.GetSRV(), w, h);
        }
    }

private:
    void CancelSceneLoad()
    {
        m_levelLoad.Cancel();
    }

    void BeginAsyncSceneLoad(const std::string& scenePath)
    {
        m_levelLoad.Cancel();
        m_sceneLoadError.clear();
        m_completeSceneLoadNextFrame = false;
        m_loadingScenePath = scenePath;
        m_levelLoad.Start(scenePath, GetAssets().CaptureSnapshot());
    }

    void DrawLoadingUI()
    {
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

        const SE::LevelLoadStatus status = m_levelLoad.GetStatus();
        const char* phaseText = "READING LEVEL DATA";
        switch (status.state)
        {
        case SE::LevelLoadState::Preparing: phaseText = "PREPARING LEVEL DEPENDENCIES"; break;
        case SE::LevelLoadState::Uploading: phaseText = "UPLOADING LEVEL RESOURCES"; break;
        case SE::LevelLoadState::Complete:  phaseText = "READY"; break;
        case SE::LevelLoadState::Failed:    phaseText = "LOAD FAILED"; break;
        case SE::LevelLoadState::Cancelled: phaseText = "LOAD CANCELLED"; break;
        default: break;
        }

        const float progress = std::clamp(status.progress, 0.0f, 1.0f);
        const std::string& path = m_loadingScenePath.empty() ? m_initialScenePath
                                                             : m_loadingScenePath;
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float overlayHeight = 64.0f;
        constexpr float barHeight = 6.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x,
                                      viewport->Pos.y + viewport->Size.y - overlayHeight),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, overlayHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 6.0f));
        ImGui::Begin("##LevelLoadingStrip", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoInputs);

        // blue (#00A0DE), with a dark shadow so the
        // borderless overlay remains legible over bright level backgrounds.
        constexpr ImU32 blue = IM_COL32(0, 160, 222, 255);
        constexpr ImU32 textShadow = IM_COL32(0, 18, 32, 230);
        const ImVec4 blueFloat(0.0f, 160.0f / 255.0f, 222.0f / 255.0f, 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto drawShadow = [&](const char* text) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawList->AddText(ImVec2(pos.x + 2.0f, pos.y + 2.0f), textShadow, text);
        };

        char levelText[256];
        sprintf_s(levelText, "// LOADING LEVEL: %s", getFilename(path).c_str());
        drawShadow(levelText);
        ImGui::TextColored(blueFloat, "%s", levelText);
        if (m_sceneLoadError.empty())
        {
            drawShadow(phaseText);
            ImGui::TextColored(blueFloat, "%s", phaseText);
        }
        else
        {
            char errorText[256];
            sprintf_s(errorText, "%s  //  [R] RETRY   [ESC] QUIT",
                      m_sceneLoadError.c_str());
            drawShadow(errorText);
            ImGui::TextColored(ImVec4(1.0f, 0.18f, 0.08f, 1.0f), "%s", errorText);
        }


        const ImVec2 barMin(viewport->Pos.x,
                            viewport->Pos.y + viewport->Size.y - barHeight);
        const ImVec2 barMax(viewport->Pos.x + viewport->Size.x * progress,
                            viewport->Pos.y + viewport->Size.y);
        drawList->AddRectFilled(barMin, barMax, blue);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Scene-driven firelight: modulate live light color from the base descriptor
    // color so UI edits target the descriptor and never fight the modulation.
    void UpdatePointLightFlicker(float totalTime)
    {
        for (int i = 0; i < m_lights.numLights && i < static_cast<int>(m_pointLightDescs.size()); ++i)
        {
            const auto& source = m_pointLightDescs[i];
            float factor = 1.0f;
            if (source.flicker)
            {
                const float phase = static_cast<float>(source.flickerSeed) * 1.618034f;
                const float primary = sinf(totalTime * source.flickerFrequency * XM_2PI + phase);
                const float secondary = sinf(totalTime * source.flickerFrequency * 1.713f * XM_2PI + phase * 2.37f);
                factor += source.flickerAmplitude * (primary * 0.65f + secondary * 0.35f);
                factor = (std::max)(0.1f, factor);
            }
            m_lights.lights[i].color = {
                source.color[0] * factor, source.color[1] * factor, source.color[2] * factor
            };
        }
    }

    void DrawPointLightsPanel()
    {
        ImGui::SliderInt("Active", &m_lights.numLights, 0, 8);
        for (int i = 0; i < m_lights.numLights; ++i)
        {
            ImGui::PushID(i);
            char label[24];
            sprintf_s(label, "Point Light %d", i + 1);
            if (ImGui::TreeNode(label))
            {
                ImGui::DragFloat3("Position", &m_lights.lights[i].position.x, 1.0f, -1000.0f, 1000.0f);
                if (i < static_cast<int>(m_pointLightDescs.size()))
                {
                    // Scene-driven light: edit the base color; the per-frame
                    // flicker factor is applied on top of it.
                    auto& source = m_pointLightDescs[i];
                    ImGui::ColorEdit3("Color", source.color.data(),
                                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    ImGui::Checkbox("Flicker", &source.flicker);
                    if (source.flicker)
                    {
                        ImGui::SliderFloat("Amplitude", &source.flickerAmplitude, 0.0f, 1.0f);
                        ImGui::SliderFloat("Frequency", &source.flickerFrequency, 0.5f, 20.0f);
                    }
                }
                else
                {
                    ImGui::ColorEdit3("Color", &m_lights.lights[i].color.x,
                                      ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                }
                ImGui::SliderFloat("Radius",  &m_lights.lights[i].radius, 0.5f, 500.0f);
                if (i < 2)
                    ImGui::Checkbox("Cast Shadow", &m_lightCastsShadow[i]);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void DrawUI(XMMATRIX view, XMMATRIX proj)
    {
        const bool sceneLoading = m_initialSceneLoad || !m_loadingScenePath.empty();
        if (sceneLoading)
        {
            DrawLoadingUI();
            return;
        }

        // Full-viewport dockspace
        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        // Build default layout only on very first run (no saved layout)
        if (m_firstFrame)
        {
            m_firstFrame = false;

            // Check if imgui.ini exists — if not, build a programmatic default
            FILE* f = nullptr;
            fopen_s(&f, "imgui.ini", "r");
            bool hasIni = (f != nullptr);
            if (f) fclose(f);

            if (!hasIni)
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                // Right sidebar (~14% width)
                ImGuiID dockCenter = dockspaceId;
                ImGuiID dockRight;
                ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.14f, &dockRight, &dockCenter);

                // Split right sidebar into 4 vertical sections
                ImGuiID dockTop, dockRest;
                ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.12f, &dockTop, &dockRest);
                ImGuiID dockMid, dockLower;
                ImGui::DockBuilderSplitNode(dockRest, ImGuiDir_Up, 0.35f, &dockMid, &dockLower);
                ImGuiID dockBottom, dockFinal;
                ImGui::DockBuilderSplitNode(dockLower, ImGuiDir_Up, 0.55f, &dockBottom, &dockFinal);

                // Top tabs: Scene active (last = default)
                ImGui::DockBuilderDockWindow("Camera", dockTop);
                ImGui::DockBuilderDockWindow("Scene",  dockTop);

                // Mid tabs: Particles active (last = default)
                ImGui::DockBuilderDockWindow("Physics",   dockMid);
                ImGui::DockBuilderDockWindow("Particles", dockMid);

                // Bottom tabs: Lighting active (last = default)
                ImGui::DockBuilderDockWindow("Material", dockBottom);
                ImGui::DockBuilderDockWindow("Lighting", dockBottom);

                // Final row: Post Processing
                ImGui::DockBuilderDockWindow("Post Processing", dockFinal);

                ImGui::DockBuilderFinish(dockspaceId);
            }
        }

        // On frames 2-4, force focus on desired default tabs one at a time
        m_frameCount++;
        if (m_frameCount == 2)
            ImGui::SetWindowFocus("Scene");
        else if (m_frameCount == 3)
            ImGui::SetWindowFocus("Particles");
        else if (m_frameCount == 4)
            ImGui::SetWindowFocus("Lighting");

        // --- Input ---
        ImGui::Begin("Input");
        const SE::GamepadState& gp = GetInput().GetGamepad(0);
        if (gp.connected)
        {
            ImGui::Text("Controller 0  connected");
            ImGui::Separator();
            ImGui::Text("Left  stick  (%.2f, %.2f)", gp.leftX,  gp.leftY);
            ImGui::Text("Right stick  (%.2f, %.2f)", gp.rightX, gp.rightY);
            ImGui::Text("LT  %.2f   RT  %.2f",       gp.leftTrigger, gp.rightTrigger);
        }
        else
        {
            ImGui::TextDisabled("Controller 0  not connected");
        }
        ImGui::End();

        // HUD — FPS + asset stats rendered directly on screen (no window chrome)
        {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            char buf[128];
            sprintf_s(buf, "%.1f fps  |  %.2f ms",
                GetClock().GetFPS(), GetClock().GetDeltaTime() * 1000.0f);
            dl->AddText(ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 220), buf);
            uint32_t subMeshCount = 0;
            for (const auto& loaded : m_loadedMeshes)
                if (loaded.mesh)
                    subMeshCount += loaded.mesh->GetSubMeshCount();
            sprintf_s(buf, "meshes:%u  textures:%u  submeshes:%u",
                GetAssets().CachedMeshCount(), GetAssets().CachedTextureCount(),
                subMeshCount);
            dl->AddText(ImVec2(10.0f, 26.0f), IM_COL32(200, 200, 200, 180), buf);
        }

        // --- Scene Picker ---
        auto getFilename = [](const std::string& path) -> std::string {
            auto pos = path.find_last_of("\\/");
            return (pos != std::string::npos) ? path.substr(pos + 1) : path;
        };

        ImGui::Begin("Scene");
        if (sceneLoading)
            ImGui::BeginDisabled();
        if (!m_sceneFiles.empty())
        {
            if (ImGui::BeginCombo("Scene File", getFilename(m_sceneFiles[m_selectedScene]).c_str()))
            {
                for (int i = 0; i < (int)m_sceneFiles.size(); ++i)
                {
                    bool selected = (i == m_selectedScene);
                    if (ImGui::Selectable(getFilename(m_sceneFiles[i]).c_str(), selected))
                    {
                        if (i != m_selectedScene)
                        {
                            m_pendingSceneLoad = m_sceneFiles[i];
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Reload"))
                m_pendingSceneLoad = m_sceneFiles[m_selectedScene];
            ImGui::SameLine();
            if (ImGui::Button("Rescan"))
            {
                m_sceneFiles = SE::SceneLoader::ScanSceneDirectory("Assets/Scenes");
                RefreshParticlePresets();
            }
        }
        if (sceneLoading)
            ImGui::EndDisabled();
        ImGui::Text("Active: %s", m_currentDesc.name.c_str());
        if (!m_sceneLoadError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_sceneLoadError.c_str());
        ImGui::End();

        // --- Camera ---
        ImGui::Begin("Camera");
        ImGui::Text("[Tab] to switch mode");
        if (m_camCtrl.GetMode() == SE::CameraController::Mode::FPS)
        {
            ImGui::Text("FPS — WASD move  RMB look  Space jump");
            ImGui::SliderFloat("Move Speed", &m_cc.moveSpeed, 1.0f, 20.0f);
            ImGui::SliderFloat("Jump Speed", &m_cc.jumpSpeed, 2.0f, 20.0f);
            ImGui::Text("pos (%.1f, %.1f, %.1f)  vy=%.2f  %s",
                m_cc.position.x, m_cc.position.y, m_cc.position.z,
                m_cc.velY, m_cc.isGrounded ? "GROUNDED" : "air");
            ImGui::Text("contact n (%.2f, %.2f, %.2f)  pvxz (%.1f, %.1f)",
                m_cc.contactNormal.x, m_cc.contactNormal.y, m_cc.contactNormal.z,
                m_cc.physVelX, m_cc.physVelZ);
        }
        else
        {
            ImGui::Text("Free Fly — WASD/QE move  RMB look  Shift fast");
            ImGui::SliderFloat("Move Speed",  &m_camCtrl.freeFly.moveSpeed,   1.0f, 200.0f);
            ImGui::SliderFloat("Sensitivity", &m_camCtrl.freeFly.sensitivity, 0.05f, 0.5f);
        }
        ImGui::SliderFloat("Near Z", &m_camera->nearZ, 0.001f, 10.0f, "%.3f");
        ImGui::SliderFloat("Far Z", &m_camera->farZ, 100.0f, 20000.0f, "%.0f");
        ImGui::Text("Eye (%.1f, %.1f, %.1f)",
            m_camera->eye.x, m_camera->eye.y, m_camera->eye.z);
        ImGui::End();

        // --- Material ---
        ImGui::Begin("Material");
        ImGui::ColorEdit3("Albedo Tint",      m_matTint);
        ImGui::SliderFloat("Roughness Scale", &m_roughnessScale, 0.0f, 2.0f);
        ImGui::SliderFloat("Metallic",        &m_metallic,       0.0f, 1.0f);
        ImGui::End();

        // --- Physics ---
        ImGui::Begin("Physics");
        ImGui::Checkbox("Show Colliders", &m_showColliders);
        ImGui::SameLine();
        ImGui::Checkbox("Raycast", &m_castRay);
        if (ImGui::Checkbox("Gravity", &m_gravityEnabled))
        {
            m_ballRigidBody->useGravity = m_gravityEnabled;
            m_cc.gravityEnabled         = m_gravityEnabled;
        }
        ImGui::Separator();
        ImGui::Text("Ball");
        ImGui::DragFloat3("Spawn pos",    &m_ballSpawn.x,                1.0f);
        ImGui::SliderFloat("Radius",      &m_ballRadius,                 0.1f, 10.0f);
        ImGui::SliderFloat("Mass",        &m_ballRigidBody->mass,        0.1f, 10.0f);
        ImGui::SliderFloat("Restitution", &m_ballRigidBody->restitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Friction",    &m_ballRigidBody->friction,    0.0f, 1.0f);
        if (ImGui::SliderFloat("Floor Y", &m_floorY, -50.0f, 50.0f))
        {
            m_physicsWorld.Clear();
            m_physicsWorld.AddSphere(m_ballTransform, m_ballRigidBody, m_ballRadius);
            m_obbFloor = SE::OBB::FromAABB({ -30.0f, m_floorY - 0.5f, -30.0f },
                                            {  30.0f, m_floorY,        30.0f });
            m_physicsWorld.AddStaticOBB(m_obbFloor, 0.6f, 0.4f);
        }
        ImGui::Text("pos (%.1f, %.1f, %.1f)",
            m_ballTransform->position.x, m_ballTransform->position.y, m_ballTransform->position.z);
        ImGui::Text("vel (%.2f, %.2f, %.2f)",
            m_ballRigidBody->velocity.x, m_ballRigidBody->velocity.y, m_ballRigidBody->velocity.z);
        if (ImGui::Button("Reset"))     ResetBall();
        ImGui::SameLine();
        if (ImGui::Button("Launch up")) m_ballRigidBody->AddImpulse({ 0.0f, 20.0f, 0.0f });
        if (m_castRay)
        {
            ImGui::Separator();
            ImGui::Text("Raycast");
            if (m_rayHitValid)
            {
                const char* what =
                    m_rayHit.kind == SE::PhysicsWorld::RaycastHit::Kind::Sphere ? "Sphere" :
                    m_rayHit.kind == SE::PhysicsWorld::RaycastHit::Kind::OBB    ? "OBB"    : "Floor";
                ImGui::Text("Hit: %s  t=%.2f", what, m_rayHit.t);
                ImGui::Text("pos  (%.1f, %.1f, %.1f)",
                    m_rayHit.point.x,  m_rayHit.point.y,  m_rayHit.point.z);
                ImGui::Text("norm (%.2f, %.2f, %.2f)",
                    m_rayHit.normal.x, m_rayHit.normal.y, m_rayHit.normal.z);
            }
            else
            {
                ImGui::TextDisabled("no hit");
            }
        }
        ImGui::End();

        // --- Lighting ---
        ImGui::Begin("Lighting");
        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Elevation",    &m_lights.elevDeg,  -90.0f, 90.0f,  "%.1f deg");
            ImGui::SliderFloat("Azimuth",      &m_lights.azimDeg, -180.0f, 180.0f, "%.1f deg");
            ImGui::SliderFloat("Intensity",    &m_lights.lightIntensity, 0.0f, 10.0f, "%.2f");
            ImGui::ColorEdit3("Light Color",   m_lights.lightColor);
            ImGui::SliderFloat("IBL Intensity", &m_lights.iblIntensity, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Diffuse IBL Saturation", &m_lights.diffuseIBLSaturation,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Shadow Bias", &m_pointShadowBias, 0.001f, 0.1f, "%.4f");
        }
        if (ImGui::CollapsingHeader("Spot Light"))
        {
            ImGui::Checkbox("Enable Spot", &m_spotLight.enabled);
            if (m_spotLight.enabled)
            {
                if (ImGui::Button("Aim From Camera"))
                {
                    XMVECTOR eye = XMLoadFloat3(&m_camera->eye);
                    XMVECTOR target = XMLoadFloat3(&m_camera->target);
                    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(target, eye));
                    m_spotLight.position = m_camera->eye;
                    XMStoreFloat3(&m_spotLight.direction, dir);
                }
                ImGui::DragFloat3("Spot Pos", &m_spotLight.position.x, 0.5f, -500.0f, 500.0f);
                ImGui::DragFloat3("Spot Dir", &m_spotLight.direction.x, 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("Spot Color", &m_spotLight.color.x);
                ImGui::SliderFloat("Spot Range", &m_spotLight.range, 1.0f, 200.0f);
                ImGui::SliderFloat("Spot Inner", &m_spotLight.innerAngle, 1.0f, m_spotLight.outerAngle - 0.5f);
                ImGui::SliderFloat("Spot Outer", &m_spotLight.outerAngle, m_spotLight.innerAngle + 0.5f, 89.0f);
                ImGui::SliderFloat("Spot Intensity", &m_spotLight.intensity, 0.1f, 50.0f);
                ImGui::SliderFloat("Spot Volumetric", &m_spotLight.volumetricIntensity, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Spot Bias", &m_spotLight.shadowBias, 0.0001f, 0.01f, "%.4f");
            }
        }
        if (ImGui::CollapsingHeader("Point Lights"))
            DrawPointLightsPanel();
        if (ImGui::CollapsingHeader("Debug"))
        {
            ImGui::Checkbox("Show Shadow Factor", &m_debugShadow);
            int dlm = (int)m_lights.debugLightMode;
            ImGui::RadioButton("Normal",    &dlm, 0); ImGui::SameLine();
            ImGui::RadioButton("Force Lit", &dlm, 1); ImGui::SameLine();
            ImGui::RadioButton("NdotL",     &dlm, 2); ImGui::SameLine();
            ImGui::RadioButton("Roughness", &dlm, 3);
            m_lights.debugLightMode = (float)dlm;
        }
        ImGui::End();

        // --- Post Processing ---
        ImGui::Begin("Post Processing");
        if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat("Exposure", &m_toneMap.exposure, 0.01f, 10.0f, "%.2f");
            {
                int op = static_cast<int>(m_toneMap.op);
                ImGui::RadioButton("Reinhard", &op, 0); ImGui::SameLine();
                ImGui::RadioButton("ACES",     &op, 1);
                m_toneMap.op = static_cast<SE::ToneMap::Operator>(op);
            }
            ImGui::Checkbox("Gamma Correct", &m_toneMap.gammaCorrect);
        }
        if (ImGui::CollapsingHeader("Bloom"))
        {
            ImGui::Checkbox("Enable Bloom", &m_bloom.enabled);
            if (m_bloom.enabled)
            {
                ImGui::SliderFloat("Threshold", &m_bloom.threshold, 0.0f, 4.0f,  "%.2f");
                ImGui::SliderFloat("Strength",  &m_bloom.intensity, 0.0f, 0.5f,  "%.3f");
                ImGui::SliderFloat("Scatter",   &m_bloom.scatter,   0.0f, 1.0f,  "%.2f");
            }
        }
        if (ImGui::CollapsingHeader("SSAO"))
        {
            ImGui::Checkbox("Enable SSAO", &m_ssao.enabled);
            if (m_ssao.enabled)
            {
                ImGui::SliderFloat("AO Radius", &m_ssao.radius, 0.01f, 5.0f, "%.2f");
                ImGui::SliderFloat("AO Bias",   &m_ssao.bias,   0.001f, 0.1f, "%.3f");
                ImGui::SliderFloat("AO Power",  &m_ssao.power,  0.5f, 8.0f,   "%.1f");
            }
        }
        if (ImGui::CollapsingHeader("Volumetric Fog"))
        {
            ImGui::Checkbox("Enable Fog", &m_volFog.enabled);
            if (m_volFog.enabled)
            {
                ImGui::SliderFloat("Density",       &m_volFog.density,          0.0f, 0.5f,  "%.3f");
                ImGui::SliderFloat("Anisotropy",    &m_volFog.anisotropy,      -0.9f, 0.95f, "%.2f");
                ImGui::SliderFloat("Sun Scatter",   &m_volFog.scatterIntensity, 0.0f, 5.0f,  "%.2f");
                ImGui::SliderInt("Steps",           &m_volFog.steps,            8, 128);
                ImGui::SliderFloat("Max Distance",  &m_volFog.maxDistance,      5.0f, 300.0f, "%.0f");
                ImGui::ColorEdit3("Fog Tint",       m_volFog.ambientColor);
                ImGui::SliderFloat("Ambient Fog",   &m_volFog.ambientIntensity, 0.0f, 2.0f,  "%.3f");
                ImGui::SliderFloat("Height Falloff",&m_volFog.heightFalloff,    0.0f, 1.0f,  "%.3f");
                ImGui::SliderFloat("Height Base",   &m_volFog.heightBase,      -10.0f, 20.0f, "%.1f");
                ImGui::SliderFloat("Noise",         &m_volFog.noiseStrength,    0.0f, 1.0f,  "%.2f");
                ImGui::SliderFloat("Noise Scale",   &m_volFog.noiseScale,       0.001f, 1.0f, "%.3f");
                ImGui::SliderFloat("Noise Drift",   &m_volFog.noiseSpeed,       0.0f, 2.0f,  "%.2f");
                ImGui::SliderFloat("Near Clear Radius", &m_volFog.nearClearDistance, 0.0f, 1000.0f, "%.1f");
                ImGui::SliderFloat("Distance Density", &m_volFog.distanceDensity, 0.0f, 8.0f, "%.2f");
            }
        }
        if (ImGui::CollapsingHeader("FXAA"))
        {
            ImGui::Checkbox("Enable FXAA", &m_fxaa.enabled);
            if (m_fxaa.enabled)
            {
                ImGui::SliderFloat("Subpix Quality", &m_fxaa.subpixQuality, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Edge Threshold", &m_fxaa.edgeThreshold, 0.063f, 0.333f, "%.3f");
                ImGui::SliderFloat("Edge Min",       &m_fxaa.edgeThresholdMin, 0.0f, 0.1f, "%.4f");
            }
        }
        ImGui::End();

        // --- Particles ---
        ImGui::Begin("Particles");
        ImGui::Text("Emitters: %d", (int)m_particleSystems.size());
        ImGui::SameLine();
        if (ImGui::Button("Rescan Presets"))
            RefreshParticlePresets();
        if (ImGui::Button("Fire All Single Emitters"))
        {
            for (auto& emitter : m_particleSystems)
            {
                if (emitter->config.emitMode == 1)
                    emitter->Burst(8);
            }
        }
        ImGui::Separator();
        for (int i = 0; i < (int)m_particleSystems.size(); ++i)
        {
            auto& ps = *m_particleSystems[i];
            ImGui::PushID(i);
            char label[32];
            sprintf_s(label, "Emitter %d", i);
            if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Preset");
                if (!m_particlePresetFiles.empty())
                {
                    int& selected = m_selectedParticlePreset[i];
                    std::string previewName = (selected >= 0 && selected < (int)m_particlePresetFiles.size())
                        ? getFilename(m_particlePresetFiles[selected])
                        : "Select preset...";
                    if (ImGui::BeginCombo("Preset File", previewName.c_str()))
                    {
                        for (int p = 0; p < (int)m_particlePresetFiles.size(); ++p)
                        {
                            bool isSelected = (p == selected);
                            if (ImGui::Selectable(getFilename(m_particlePresetFiles[p]).c_str(), isSelected))
                                selected = p;
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (selected >= 0 && selected < (int)m_particlePresetFiles.size())
                    {
                        if (ImGui::Button("Load Preset"))
                        {
                            SE::SceneDescriptor::ParticleEmitterDesc preset;
                            if (SE::SceneLoader::LoadParticlePreset(m_particlePresetFiles[selected], preset))
                            {
                                int oldMax = ps.config.maxParticles;
                                ApplyParticleDescToSystem(preset, ps);
                                m_particleEmitterDescs[i] = preset;
                                if (!preset.texture.empty())
                                {
                                    auto toW = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
                                    ps.SetTexture(GetAssets().FindTexture(toW(preset.texture)));
                                }
                                if (oldMax != ps.config.maxParticles)
                                    ps.Init(GetRenderer().GetDevice(), GetShaders());
                            }
                        }
                    }
                }
                else
                {
                    ImGui::TextDisabled("No presets in Assets/Particles/Presets");
                }
                ImGui::Separator();

                ImGui::Text("Emission");
                ImGui::Checkbox("Enable", &ps.enabled);
                ImGui::Combo("Mode", &ps.config.emitMode, "Continuous\0Single\0");
                if (ps.config.emitMode == 0)
                    ImGui::SliderFloat("Emit Rate", &ps.config.emitRate, 1.0f, 500.0f);
                else
                {
                    ImGui::TextDisabled("Emit Rate");
                    if (ImGui::Button("Press to Fire"))
                        ps.Burst(8);
                }
                ImGui::Separator();

                ImGui::Text("Transform");
                ImGui::DragFloat3("Position", &ps.config.position.x, 0.1f, -50.0f, 50.0f);
                ImGui::Combo("Spawn Shape", &ps.config.spawnShape, "Sphere\0Box\0");
                if (ps.config.spawnShape == 1)
                    ImGui::DragFloat3("Box Extents", &ps.config.spawnBoxExtents.x, 0.1f, 0.0f, 50.0f);
                else
                    ImGui::SliderFloat("Spawn Radius", &ps.config.spawnRadius, 0.0f, 5.0f);
                ImGui::SliderFloat("Aspect Ratio", &ps.config.aspectRatio, 0.5f, 4.0f);
                ImGui::SliderFloat("Rotation (deg)", &ps.config.rotationDegrees, -180.0f, 180.0f);
                ImGui::Checkbox("Random Rotation", &ps.config.randomRotation);
                ImGui::Separator();

                ImGui::Text("Lifetime & Size");
                ImGui::SliderFloat("Life Min",     &ps.config.lifetimeMin, 0.1f, 10.0f);
                ImGui::SliderFloat("Life Max",     &ps.config.lifetimeMax, 0.1f, 10.0f);
                ImGui::SliderFloat("Size Start",   &ps.config.sizeStart,   0.01f, 2.0f);
                ImGui::SliderFloat("Size End",     &ps.config.sizeEnd,     0.0f, 2.0f);
                ImGui::Separator();

                ImGui::Text("Motion");
                ImGui::DragFloat3("Velocity Min",  &ps.config.velocityMin.x, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat3("Velocity Max",  &ps.config.velocityMax.x, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat3("Gravity",       &ps.config.gravity.x, 0.1f, -20.0f, 20.0f);
                ImGui::SliderFloat("Drag", &ps.config.drag, 0.0f, 5.0f);
                ImGui::Separator();

                ImGui::Text("Color");
                ImGui::ColorEdit4("Color Start",   &ps.config.colorStart.x);
                ImGui::ColorEdit4("Color End",     &ps.config.colorEnd.x);
                ImGui::Separator();

                ImGui::Text("Atlas");
                ImGui::InputInt("Columns", &ps.config.atlasColumns);
                ImGui::InputInt("Rows",    &ps.config.atlasRows);
                ImGui::InputInt("Frames",  &ps.config.atlasFrameCount);
                ImGui::SliderFloat("Speed", &ps.config.atlasSpeed, 0.1f, 5.0f);
                ImGui::SliderFloat("Texture Rotation (deg)", &ps.config.textureRotationDegrees, -180.0f, 180.0f);
                ImGui::Separator();

                ImGui::Text("Render");
                ImGui::Combo("Blend Mode", &ps.config.blendMode, "Additive\0Alpha Blend\0");
                ImGui::Combo("Shader Mode", &ps.config.shaderMode, "Textured\0Ember\0");
                ImGui::SliderFloat("Soft Distance", &ps.config.softDistance, 0.0f, 3.0f);
                ImGui::SliderFloat("Tracer Stretch", &ps.config.tracerStretch, 0.0f, 0.25f);
                ImGui::SliderFloat("Lighting Influence", &ps.config.lightingInfluence, 0.0f, 1.0f);
            }
            ImGui::PopID();
        }
        ImGui::End();

        // Viewport light indicators
        {
            XMMATRIX    vp = XMMatrixMultiply(view, proj);
            float       sw = (float)GetWindow().GetWidth();
            float       sh = (float)GetWindow().GetHeight();
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            auto project = [&](XMVECTOR wp, float& sx, float& sy) -> bool
            {
                XMVECTOR cp = XMVector4Transform(wp, vp);
                float    cw = XMVectorGetW(cp);
                if (cw <= 0.0f) return false;
                sx = ( XMVectorGetX(cp) / cw * 0.5f + 0.5f) * sw;
                sy = (-XMVectorGetY(cp) / cw * 0.5f + 0.5f) * sh;
                return true;
            };

            // Sun
            {
                float er = XMConvertToRadians(m_lights.elevDeg);
                float ar = XMConvertToRadians(m_lights.azimDeg);
                XMVECTOR dir    = XMVectorSet(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar), 0.0f);
                XMVECTOR eyeV   = XMVectorSet(m_camera->eye.x, m_camera->eye.y, m_camera->eye.z, 1.0f);
                XMVECTOR sunPos = XMVectorSetW(XMVectorAdd(eyeV, XMVectorScale(dir, 80.0f)), 1.0f);
                float sx, sy;
                if (project(sunPos, sx, sy))
                {
                    dl->AddCircleFilled({ sx, sy }, 10.0f, IM_COL32(255, 220, 50, 255));
                    for (int r = 0; r < 8; ++r)
                    {
                        float a = r * 3.14159f / 4.0f;
                        dl->AddLine({ sx + cosf(a) * 12.0f, sy + sinf(a) * 12.0f },
                                    { sx + cosf(a) * 17.0f, sy + sinf(a) * 17.0f },
                                    IM_COL32(255, 220, 50, 200), 1.5f);
                    }
                    dl->AddText({ sx + 14.0f, sy - 7.0f }, IM_COL32_WHITE, "Sun");
                }
            }

            // Point lights
            for (int i = 0; i < m_lights.numLights; ++i)
            {
                const auto& pl = m_lights.lights[i];
                float sx, sy;
                if (!project(XMVectorSet(pl.position.x, pl.position.y, pl.position.z, 1.0f), sx, sy))
                    continue;
                ImU32 fill = ImGui::ColorConvertFloat4ToU32({ pl.color.x, pl.color.y, pl.color.z, 1.0f });
                dl->AddCircleFilled({ sx, sy }, 7.0f, fill);
                dl->AddCircle({ sx, sy }, 8.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
                char tag[8];
                sprintf_s(tag, "PL%d", i + 1);
                dl->AddText({ sx + 11.0f, sy - 7.0f }, IM_COL32_WHITE, tag);
            }
        }
    }

    SE::SkyboxRenderer                       m_skybox;
    SE::AssetHandle<SE::Texture2D>           m_environmentTexture;
    SE::EnvironmentLighting                  m_environmentLighting;
    SE::LightEnvironment                     m_lights;
    SE::ForwardPipeline                      m_pipeline;
    SE::CascadedShadowMap                    m_shadowMap;
    struct LoadedMesh
    {
        SE::SceneDescriptor::MeshEntry         def;
        SE::AssetHandle<SE::Mesh>              mesh;
        std::vector<SE::ForwardPipeline::SubMat> materials;
        SE::TransformComponent*                transform = nullptr;
    };
    std::vector<LoadedMesh> m_loadedMeshes;

    struct LoadedObject
    {
        SE::SceneDescriptor::SceneObject def;
        SE::ForwardPipeline::SubMat      mat;
    };
    std::vector<LoadedObject> m_sceneObjects;

    SE::LevelWorld       m_world;
    SE::CameraComponent* m_camera  = nullptr;
    SE::CameraController m_camCtrl;

    float m_matTint[3]     = { 1.0f, 1.0f, 1.0f };
    float m_roughnessScale = 1.0f;
    float m_metallic       = 1.0f; // global scale on texture metallic; 1 = texture-driven

    SE::PhysicsWorld        m_physicsWorld;
    SE::TransformComponent* m_ballTransform = nullptr;
    SE::RigidBodyComponent* m_ballRigidBody = nullptr;
    XMFLOAT3                m_ballSpawn     = { 0.0f, 30.0f, 0.0f };
    float                   m_ballRadius    = 1.0f;
    float                   m_floorY        = 0.0f;
    SE::OBB                 m_obbFloor;
    SE::CharacterController m_cc;

    bool                         m_gravityEnabled    = true;
    bool                         m_showColliders     = false;
    bool                         m_castRay           = false;
    bool                         m_debugShadow       = false;
    bool                         m_firstFrame        = true;
    int                          m_frameCount        = 0;
    SE::RenderTarget             m_forwardHDR_RT;
    SE::RenderTarget             m_normalRT;
    SE::ToneMap                  m_toneMap;
    SE::PointShadowMap           m_pointShadowMaps[2];
    SE::Bloom                    m_bloom;
    SE::SSAO                     m_ssao;
    SE::VolumetricFog            m_volFog;
    SE::FXAA                     m_fxaa;
    SE::RenderTarget             m_ldrRT;
    SE::RenderTarget             m_planarReflectionRT;
    int                          m_planarObjectIndex = -1;
    float                        m_planarReflectionY = 0.0f;
    float                        m_planarReflectionStrength = 1.0f;
    float                        m_planarReflectionScale = 0.5f;
    DirectX::XMMATRIX            m_planarReflectionViewProj = DirectX::XMMatrixIdentity();
    SE::SpotLight                m_spotLight;
    std::vector<std::unique_ptr<SE::ParticleSystem>> m_particleSystems;
    DirectX::XMMATRIX            m_cachedProj = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX            m_cachedView = DirectX::XMMatrixIdentity();
    bool                         m_lightCastsShadow[8] = { true };
    std::vector<SE::SceneDescriptor::PointLightDesc> m_pointLightDescs;
    float                        m_pointShadowBias       = 0.015f;
    SE::PhysicsWorld::RaycastHit m_rayHit        = {};
    bool                         m_rayHitValid   = false;

    // Scene loader state
    std::vector<std::string> m_sceneFiles;
    int                      m_selectedScene    = 0;
    std::string              m_currentScenePath;
    SE::SceneDescriptor      m_currentDesc;
    std::string              m_pendingSceneLoad;
    std::string              m_loadingScenePath;
    std::string              m_initialScenePath;
    std::string              m_sceneLoadError;
    std::vector<SE::LevelDiagnostic> m_levelDiagnostics;
    bool                     m_initialSceneLoad = false;
    SE::LevelLoadOperation   m_levelLoad;
    bool                     m_completeSceneLoadNextFrame = false;
    std::vector<std::string> m_particlePresetFiles;
    std::vector<int>         m_selectedParticlePreset;
    std::vector<SE::SceneDescriptor::ParticleEmitterDesc> m_particleEmitterDescs;
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Set working directory to project root (walk up from exe location until we find Engine/ and Assets/)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 5; ++i)
        {
            if (std::filesystem::exists(dir / "Engine") && std::filesystem::exists(dir / "Assets"))
            {
                SetCurrentDirectoryW(dir.c_str());
                break;
            }
            dir = dir.parent_path();
        }
    }

    SE::WindowDesc desc;
    desc.title  = L"FoxEngine";
    desc.width  = 1280;
    desc.height = 720;

    // Parse --scene argument
    std::string scenePath;
    if (lpCmdLine && strlen(lpCmdLine) > 0)
    {
        std::string args(lpCmdLine);
        auto pos = args.find("--scene");
        if (pos != std::string::npos)
        {
            pos += 7; // skip "--scene"
            while (pos < args.size() && (args[pos] == ' ' || args[pos] == '=')) ++pos;
            auto end = args.find(' ', pos);
            scenePath = args.substr(pos, end - pos);
            // If just a filename, prepend Assets/Scenes/
            if (scenePath.find('/') == std::string::npos && scenePath.find('\\') == std::string::npos)
                scenePath = "Assets/Scenes/" + scenePath;
        }
    }

    TestScene scene;
    if (!scene.Initialize(desc)) return 1;
    if (!scene.Setup(scenePath)) return 1;
    scene.Run();
    scene.Shutdown();
    return 0;
}
