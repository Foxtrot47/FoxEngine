#include "Engine/Scene/SceneRuntime.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Logger.h"
#include "Engine/Physics/Ray.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneLoader.h"

using namespace DirectX;

namespace SE {

namespace {

bool NearlyEqual(float a, float b, float eps = 0.0001f)
{
    return std::fabs(a - b) <= eps;
}

bool EqualFloat3(const std::array<float, 3>& a, const std::array<float, 3>& b, float eps = 0.0001f)
{
    return NearlyEqual(a[0], b[0], eps) && NearlyEqual(a[1], b[1], eps) && NearlyEqual(a[2], b[2], eps);
}

bool EqualFloat4(const std::array<float, 4>& a, const std::array<float, 4>& b, float eps = 0.0001f)
{
    return NearlyEqual(a[0], b[0], eps) && NearlyEqual(a[1], b[1], eps) &&
           NearlyEqual(a[2], b[2], eps) && NearlyEqual(a[3], b[3], eps);
}

bool IsSameParticlePreset(const SceneDescriptor::ParticleEmitterDesc& a,
                          const SceneDescriptor::ParticleEmitterDesc& b)
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

std::vector<ShadowSubMesh> BuildShadowSubMeshes(
    const std::vector<ForwardPipeline::SubMat>& materials)
{
    std::vector<ShadowSubMesh> shadowSubMeshes;
    shadowSubMeshes.reserve(materials.size());
    for (const auto& material : materials)
    {
        ShadowSubMesh subMesh;
        subMesh.cast = material.alphaMode != AlphaMode::Transparent;
        if (material.alphaMode == AlphaMode::Cutout &&
            material.albedo && material.albedo->HasAlpha())
        {
            subMesh.albedo = material.albedo->GetSRV();
            subMesh.alphaCutoff = material.alphaCutoff;
        }
        shadowSubMeshes.push_back(subMesh);
    }
    return shadowSubMeshes;
}

DirectX::XMMATRIX SceneObjectModel(const SceneDescriptor::SceneObject& object)
{
    using namespace DirectX;
    const XMMATRIX translate = XMMatrixTranslation(
        object.position[0], object.position[1], object.position[2]);
    if (object.type == SceneDescriptor::SceneObject::Type::Plane)
        return XMMatrixScaling(object.halfSizeX, 1.0f, object.halfSizeZ) * translate;
    return XMMatrixScaling(object.radius, object.radius, object.radius) * translate;
}

} // namespace

SceneRuntime::~SceneRuntime()
{
    m_levelLoad.Cancel();
}

bool SceneRuntime::Init(Renderer& renderer, ShaderLibrary& shaders, AssetManager& assets,
                        InputManager& input, HWND hwnd, uint32_t width, uint32_t height)
{
    m_renderer = &renderer;
    m_shaders  = &shaders;
    m_assets   = &assets;
    m_input    = &input;
    m_hwnd     = hwnd;
    m_outputWidth  = width;
    m_outputHeight = height;

    ID3D11Device* device = renderer.GetDevice();

    if (!skybox.Init(device, renderer.GetStateCache(), shaders)) return false;
    if (!environmentLighting.Init(device, shaders)) return false;
    if (!lights.Init(device))                return false;
    if (!pipeline.Init(device, assets, shaders)) return false;
    if (!shadowMap.Init(device, shaders, 2048)) return false;

    // Forward HDR render target with readable depth for screen-space effects.
    if (!m_forwardHDR_RT.Init(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            /*withDepth=*/true, /*depthReadable=*/true))
        return false;

    // Normal + roughness MRT target (view-space normal xyz + roughness w)
    if (!m_normalRT.Init(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return false;

    if (!toneMap.Init(device, shaders)) return false;

    // Point shadow maps — 2 slots, 256x256 per cube face
    for (int i = 0; i < 2; ++i)
        if (!pointShadowMaps[i].Init(device, shaders, 256)) return false;

    if (!bloom.Init(device, shaders, width, height)) return false;
    if (!ssao.Init(device, shaders, width, height)) return false;
    if (!volFog.Init(device, shaders, width, height)) return false;
    if (!fxaa.Init(device, shaders)) return false;

    // Velocity buffer (UV-space motion vectors) + TAA
    if (!m_velocityRT.Init(device, width, height, DXGI_FORMAT_R16G16_FLOAT))
        return false;
    if (!taa.Init(device, shaders, width, height)) return false;
    fxaa.enabled = false; // temporal AA is the default AA path

    // LDR intermediate RT for FXAA input
    if (!m_ldrRT.Init(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM))
        return false;

    if (!spotLight.Init(device, shaders, 1024)) return false;

    RefreshParticlePresets();
    return true;
}

void SceneRuntime::Shutdown()
{
    // The worker may still be creating D3D resources. Join it before the
    // device is released.
    m_levelLoad.Cancel();
}

// --- Async scene loading -----------------------------------------------------

void SceneRuntime::RequestSceneLoad(const std::string& scenePath)
{
    m_pendingSceneLoad = scenePath;
    lastRequestedScenePath = scenePath;
}

void SceneRuntime::BeginAsyncSceneLoad(const std::string& scenePath)
{
    m_levelLoad.Cancel();
    sceneLoadError.clear();
    m_completeSceneLoadNextFrame = false;
    m_loadingScenePath = scenePath;
    m_levelLoad.Start(scenePath, m_assets->CaptureSnapshot());
}

bool SceneRuntime::TickSceneLoad(const std::function<bool()>& pumpMessages)
{
    bool becameReady = false;
    if (m_completeSceneLoadNextFrame)
    {
        m_completeSceneLoadNextFrame = false;
        m_loadingScenePath.clear();
        ResetTemporalHistory(); // camera jumps on scene load
        SE_LOG_INFO("Scene ready — %s", currentDesc.name.c_str());
        becameReady = true;
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
        m_levelLoad.PumpUploads(m_renderer->GetDevice(), m_renderer->GetContext(), *m_assets);
        const LevelLoadStatus operationStatus = m_levelLoad.GetStatus();
        if (operationStatus.state == LevelLoadState::Failed ||
            operationStatus.state == LevelLoadState::Cancelled)
        {
            sceneLoadError = operationStatus.failureReason.empty()
                ? "LEVEL LOAD FAILED" : operationStatus.failureReason;
            m_loadingScenePath.clear();
        }
        else if (operationStatus.state == LevelLoadState::Complete)
        {
            auto prepared = m_levelLoad.TakePreparedLevel();
            if (prepared)
            {
                const bool loaded = ApplyScene(m_loadingScenePath, std::move(*prepared),
                                               pumpMessages);
                if (!loaded)
                    sceneLoadError = "LEVEL LOAD FAILED";
                else
                    m_completeSceneLoadNextFrame = true;
            }
            m_loadingScenePath.clear();
        }
    }
    return becameReady;
}

// --- Scene apply (transactional) --------------------------------------------

bool SceneRuntime::ImportSceneMeshes(const SceneDescriptor& desc, PreparedLevel& prepared,
                                     const std::function<bool()>& pumpMessages,
                                     std::vector<LoadedMesh>& outMeshes)
{
    // Keep the active level alive until every replacement mesh has imported successfully.
    outMeshes.reserve(desc.meshes.size());
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
            sceneLoadError = "Mesh import failed or was cancelled";
            SE_LOG_WARN("Failed to load mesh: %s", meshDesc.path.c_str());
            return false;
        }

        loaded.materials = meshIndex < prepared.materials.size()
            ? std::move(prepared.materials[meshIndex])
            : std::vector<ForwardPipeline::SubMat>{};
        if (!pumpMessages())
            return false;

        for (auto& sm : loaded.materials)
        {
            sm.receiveShadows = meshDesc.receiveShadows;
            if (meshDesc.alphaCutoff > 0.0f &&
                sm.alphaMode == AlphaMode::Opaque &&
                sm.albedo && sm.albedo->HasAlpha())
            {
                sm.alphaMode   = AlphaMode::Cutout;
                sm.alphaCutoff = meshDesc.alphaCutoff;
            }
        }
        loaded.shadowSubMeshes = BuildShadowSubMeshes(loaded.materials);
        outMeshes.push_back(std::move(loaded));
    }
    return true;
}

bool SceneRuntime::CreateParticleSystems(const SceneDescriptor& desc, PreparedLevel& prepared,
                                         const std::function<bool()>& pumpMessages,
                                         std::vector<std::unique_ptr<ParticleSystem>>& outSystems,
                                         std::vector<SceneDescriptor::ParticleEmitterDesc>& outDescs)
{
    ID3D11Device* device = m_renderer->GetDevice();
    outSystems.reserve(desc.particles.size());
    outDescs.reserve(desc.particles.size());
    for (size_t particleIndex = 0; particleIndex < desc.particles.size(); ++particleIndex)
    {
        const auto& pe = desc.particles[particleIndex];
        if (!pumpMessages())
            return false;

        auto ps = std::make_unique<ParticleSystem>();
        ApplyParticleDescToSystem(pe, *ps);
        if (!ps->Init(device, *m_shaders))
        {
            sceneLoadError = "Could not initialize particle emitter";
            return false;
        }
        if (particleIndex < prepared.particleTextures.size())
            ps->SetTexture(prepared.particleTextures[particleIndex]);
        outSystems.push_back(std::move(ps));
        outDescs.push_back(pe);
    }
    return true;
}

std::vector<int> SceneRuntime::MatchParticlePresets(
    const std::vector<SceneDescriptor::ParticleEmitterDesc>& descs) const
{
    std::vector<int> selected(descs.size(), -1);
    for (int i = 0; i < (int)descs.size(); ++i)
    {
        for (int p = 0; p < (int)particlePresetFiles.size(); ++p)
        {
            SceneDescriptor::ParticleEmitterDesc preset;
            if (!SceneLoader::LoadParticlePreset(particlePresetFiles[p], preset))
                continue;
            if (IsSameParticlePreset(descs[i], preset))
            {
                selected[i] = p;
                break;
            }
        }
    }
    return selected;
}

void SceneRuntime::SetupPlanarReflection(ID3D11Device* device)
{
    m_planarObjectIndex = -1;
    m_planarReflectionScale = 0.5f;
    for (size_t objectIndex = 0; objectIndex < sceneObjects.size(); ++objectIndex)
    {
        const auto& object = sceneObjects[objectIndex].def;
        if (object.type == SceneDescriptor::SceneObject::Type::Plane && object.planarReflection)
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
        const uint32_t reflectionWidth = (std::max)(1u, static_cast<uint32_t>(m_outputWidth * m_planarReflectionScale));
        const uint32_t reflectionHeight = (std::max)(1u, static_cast<uint32_t>(m_outputHeight * m_planarReflectionScale));
        if (!m_planarReflectionRT.Init(device, reflectionWidth, reflectionHeight,
                DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, true))
        {
            SE_LOG_WARN("Planar reflection target creation failed; using environment reflection");
            m_planarObjectIndex = -1;
        }
    }
}

void SceneRuntime::ApplySceneSettings(const SceneDescriptor& desc)
{
    lights.sunEnabled = true;
    ballVisible = true;
    lights.elevDeg = desc.sun.elevation;
    lights.azimDeg = desc.sun.azimuth;
    lights.lightIntensity = desc.sun.intensity;
    lights.lightColor[0] = desc.sun.color[0];
    lights.lightColor[1] = desc.sun.color[1];
    lights.lightColor[2] = desc.sun.color[2];
    lights.iblIntensity = desc.iblIntensity;
    lights.diffuseIBLSaturation = desc.diffuseIBLSaturation;
    lights.numLights = (std::min)(static_cast<int>(desc.pointLights.size()), 8);
    pointLightDescs = desc.pointLights;
    std::fill(std::begin(lightCastsShadow), std::end(lightCastsShadow), false);
    for (int i = 0; i < lights.numLights; ++i)
    {
        const auto& light = desc.pointLights[i];
        lights.lights[i].position = { light.position[0], light.position[1], light.position[2] };
        lights.lights[i].color = { light.color[0], light.color[1], light.color[2] };
        lights.lights[i].radius = light.radius;
        lights.lights[i].enabled = true;
        lightCastsShadow[i] = light.castShadow;
    }
    toneMap.op = static_cast<ToneMap::Operator>(desc.toneMapping.op);
    toneMap.exposure = desc.toneMapping.exposure;
    toneMap.temperature = desc.colorGrading.temperature;
    toneMap.tint = desc.colorGrading.tint;
    toneMap.saturation = desc.colorGrading.saturation;
    toneMap.contrast = desc.colorGrading.contrast;
    toneMap.vignette = desc.colorGrading.vignette;
    toneMap.vignetteFeather = desc.colorGrading.vignetteFeather;
    bloom.enabled = desc.bloom.enabled;
    bloom.threshold = desc.bloom.threshold;
    bloom.intensity = desc.bloom.intensity;
    bloom.scatter = desc.bloom.scatter;
    volFog.enabled = desc.fog.enabled;
    volFog.density = desc.fog.density;
    volFog.anisotropy = desc.fog.anisotropy;
    volFog.scatterIntensity = desc.fog.scatterIntensity;
    volFog.steps = desc.fog.steps;
    volFog.maxDistance = desc.fog.maxDistance;
    std::copy(std::begin(desc.fog.ambientColor), std::end(desc.fog.ambientColor), volFog.ambientColor);
    volFog.ambientIntensity = desc.fog.ambientIntensity;
    volFog.heightFalloff = desc.fog.heightFalloff;
    volFog.heightBase = desc.fog.heightBase;
    volFog.noiseStrength = desc.fog.noiseStrength;
    volFog.noiseScale = desc.fog.noiseScale;
    volFog.noiseSpeed = desc.fog.noiseSpeed;
    volFog.nearClearDistance = desc.fog.nearClearDistance;
    volFog.distanceDensity = desc.fog.distanceDensity;
}

bool SceneRuntime::ApplyScene(const std::string& scenePath, PreparedLevel&& prepared,
                              const std::function<bool()>& pumpMessages)
{
    LevelAsset level = std::move(prepared.level);
    SceneDescriptor desc = std::move(prepared.descriptor);

    if (!pumpMessages())
        return false;

    ID3D11Device* device = m_renderer->GetDevice();

    // --- Mesh entities ---
    std::vector<LoadedMesh> newLoadedMeshes;
    if (!ImportSceneMeshes(desc, prepared, pumpMessages, newLoadedMeshes))
        return false;

    // --- Runtime world (transactional rebuild) ---
    LevelWorld newWorld;
    if (!newWorld.Instantiate(level))
    {
        sceneLoadError = "Could not instantiate level world";
        return false;
    }
    CameraComponent* newCamera = newWorld.GetPrimaryCamera();
    if (!newCamera)
    {
        Entity* cameraEntity = newWorld.GetScene().CreateEntity("Fallback Camera");
        newCamera = cameraEntity->AddComponent<CameraComponent>();
        newCamera->nearZ = desc.camera.nearZ;
        newCamera->farZ = desc.camera.farZ;
    }
    CameraController newCameraController = camCtrl;
    newCameraController.freeFly.eye      = { desc.camera.eye[0], desc.camera.eye[1], desc.camera.eye[2] };
    newCameraController.freeFly.yawDeg   = desc.camera.yaw;
    newCameraController.freeFly.pitchDeg = desc.camera.pitch;
    newCameraController.Update(0.0f, *m_input, *newCamera, m_hwnd);

    for (auto& loaded : newLoadedMeshes)
    {
        Entity* meshEntity = newWorld.FindBySourceID(loaded.def.sourceEntityID);
        if (!meshEntity || !(loaded.transform = meshEntity->GetComponent<TransformComponent>()))
        {
            sceneLoadError = "Mesh entity was not instantiated";
            return false;
        }
    }

    // Physics ball entity
    Entity* ballEnt = newWorld.GetScene().CreateEntity("PhysBall");
    auto* newBallTransform = ballEnt->AddComponent<TransformComponent>();
    auto* newBallRigidBody = ballEnt->AddComponent<RigidBodyComponent>();
    const float newBallRadius = desc.physics.ballRadius;
    const XMFLOAT3 newBallSpawn = {
        desc.physics.ballSpawn[0], desc.physics.ballSpawn[1], desc.physics.ballSpawn[2]
    };
    newBallTransform->scale = { newBallRadius, newBallRadius, newBallRadius };
    newBallTransform->position = newBallSpawn;
    newBallRigidBody->velocity = { 0.0f, 0.0f, 0.0f };

    // --- Physics world (rebuild) ---
    PhysicsWorld newPhysicsWorld;
    newPhysicsWorld.AddSphere(newBallTransform, newBallRigidBody, newBallRadius);
    const float newFloorY = desc.physics.floor.max[1];
    const OBB newFloor = OBB::FromAABB(
        { desc.physics.floor.min[0], desc.physics.floor.min[1], desc.physics.floor.min[2] },
        { desc.physics.floor.max[0], desc.physics.floor.max[1], desc.physics.floor.max[2] });
    newPhysicsWorld.AddStaticOBB(
        newFloor, desc.physics.floor.friction, desc.physics.floor.restitution);

    // Character controller
    CharacterController newCharacterController;
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
        if (!pumpMessages())
            return false;

        LoadedObject lo;
        lo.def = desc.objects[objectIndex];
        if (objectIndex < prepared.objectMaterials.size())
            lo.mat = std::move(prepared.objectMaterials[objectIndex]);
        newSceneObjects.push_back(std::move(lo));
    }

    // --- Particle emitters (from scene JSON) ---
    std::vector<std::unique_ptr<ParticleSystem>> newParticleSystems;
    std::vector<SceneDescriptor::ParticleEmitterDesc> newParticleEmitterDescs;
    if (!CreateParticleSystems(desc, prepared, pumpMessages,
                               newParticleSystems, newParticleEmitterDescs))
        return false;
    std::vector<int> newSelectedParticlePreset = MatchParticlePresets(newParticleEmitterDescs);

    // Commit only after every replacement mesh has completed its import.
    world            = std::move(newWorld);
    camera           = newCamera;
    camCtrl          = std::move(newCameraController);
    ballTransform    = newBallTransform;
    ballRigidBody    = newBallRigidBody;
    ballRadius       = newBallRadius;
    ballSpawn        = newBallSpawn;
    physicsWorld     = std::move(newPhysicsWorld);
    floorY           = newFloorY;
    obbFloor         = newFloor;
    cc               = std::move(newCharacterController);
    m_havePrevBallWorld = false;
    gravityEnabled   = desc.physics.gravity;
    sceneObjects     = std::move(newSceneObjects);
    particleSystems  = std::move(newParticleSystems);
    particleEmitterDescs = std::move(newParticleEmitterDescs);
    selectedParticlePreset = std::move(newSelectedParticlePreset);
    loadedMeshes     = std::move(newLoadedMeshes);
    environmentTexture = std::move(prepared.environment);
    iblTexture = std::move(prepared.iblEnvironment);
    levelDiagnostics = std::move(prepared.diagnostics);
    skybox.SetPanorama(environmentTexture ? environmentTexture->GetSRV() : nullptr);
    if (!environmentLighting.Build(device, m_renderer->GetContext(),
                                   iblTexture ? iblTexture->GetSRV() : nullptr))
        SE_LOG_WARN("Environment lighting generation failed; using neutral fallback");

    SetupPlanarReflection(device);
    ApplySceneSettings(desc);

    currentScenePath = scenePath;
    currentDesc      = desc;
    sceneLoadError.clear();
    return true;
}

void SceneRuntime::ApplyParticleDescToSystem(const SceneDescriptor::ParticleEmitterDesc& d, ParticleSystem& ps)
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

void SceneRuntime::SyncParticleDescFromSystem(const ParticleSystem& ps,
                                              SceneDescriptor::ParticleEmitterDesc& d)
{
    const ParticleEmitterConfig& c = ps.config;
    d.position = { c.position.x, c.position.y, c.position.z };
    d.emitRate = c.emitRate;
    d.maxParticles = c.maxParticles;
    d.lifetimeMin = c.lifetimeMin;
    d.lifetimeMax = c.lifetimeMax;
    d.velocityMin = { c.velocityMin.x, c.velocityMin.y, c.velocityMin.z };
    d.velocityMax = { c.velocityMax.x, c.velocityMax.y, c.velocityMax.z };
    d.sizeStart = c.sizeStart;
    d.sizeEnd = c.sizeEnd;
    d.colorStart = { c.colorStart.x, c.colorStart.y, c.colorStart.z, c.colorStart.w };
    d.colorEnd = { c.colorEnd.x, c.colorEnd.y, c.colorEnd.z, c.colorEnd.w };
    d.gravity = { c.gravity.x, c.gravity.y, c.gravity.z };
    d.spawnRadius = c.spawnRadius;
    d.spawnShape = c.spawnShape;
    d.spawnBoxExtents = { c.spawnBoxExtents.x, c.spawnBoxExtents.y, c.spawnBoxExtents.z };
    d.atlasColumns = c.atlasColumns;
    d.atlasRows = c.atlasRows;
    d.atlasFrameCount = c.atlasFrameCount;
    d.atlasSpeed = c.atlasSpeed;
    d.textureRotationDegrees = c.textureRotationDegrees;
    d.softDistance = c.softDistance;
    d.aspectRatio = c.aspectRatio;
    d.blendMode = c.blendMode;
    d.drag = c.drag;
    d.shaderMode = c.shaderMode;
    d.emitMode = c.emitMode;
    d.rotationDegrees = c.rotationDegrees;
    d.randomRotation = c.randomRotation;
    d.tracerStretch = c.tracerStretch;
    d.lightingInfluence = c.lightingInfluence;
}

void SceneRuntime::SyncDescriptorFromRuntime()
{
    if (!HasScene())
        return;
    SceneDescriptor& desc = currentDesc;

    // Sun + IBL
    desc.sun.elevation = lights.elevDeg;
    desc.sun.azimuth = lights.azimDeg;
    desc.sun.intensity = lights.lightIntensity;
    desc.sun.color = { lights.lightColor[0], lights.lightColor[1], lights.lightColor[2] };
    desc.iblIntensity = lights.iblIntensity;
    desc.diffuseIBLSaturation = lights.diffuseIBLSaturation;

    // Point lights: descs hold base color + flicker settings; position and
    // radius are edited on the bound lights, shadow casting in the flag array.
    desc.pointLights = pointLightDescs;
    const int syncCount =
        (std::min)(static_cast<int>(desc.pointLights.size()), lights.numLights);
    for (int i = 0; i < syncCount; ++i)
    {
        auto& light = desc.pointLights[i];
        const auto& bound = lights.lights[i];
        light.position = { bound.position.x, bound.position.y, bound.position.z };
        light.radius = bound.radius;
        light.castShadow = lightCastsShadow[i];
    }

    // Camera: the fly pose is runtime-only, but clip planes are edited.
    desc.camera.nearZ = camera->nearZ;
    desc.camera.farZ = camera->farZ;

    // Entity definitions edited in place by tooling
    desc.meshes.clear();
    for (const auto& loaded : loadedMeshes)
        desc.meshes.push_back(loaded.def);
    desc.objects.clear();
    for (const auto& object : sceneObjects)
        desc.objects.push_back(object.def);

    // Particles: live edits target ParticleSystem::config
    for (size_t i = 0; i < particleSystems.size() && i < particleEmitterDescs.size(); ++i)
        SyncParticleDescFromSystem(*particleSystems[i], particleEmitterDescs[i]);
    desc.particles = particleEmitterDescs;

    // Physics: the Floor Y slider moves the slab, thickness is preserved.
    desc.physics.gravity = gravityEnabled;
    desc.physics.ballRadius = ballRadius;
    desc.physics.ballSpawn = { ballSpawn.x, ballSpawn.y, ballSpawn.z };
    const float floorThickness = desc.physics.floor.max[1] - desc.physics.floor.min[1];
    desc.physics.floor.max[1] = floorY;
    desc.physics.floor.min[1] = floorY - floorThickness;

    // Post-process
    desc.toneMapping.op = static_cast<int>(toneMap.op);
    desc.toneMapping.exposure = toneMap.exposure;
    desc.colorGrading.temperature = toneMap.temperature;
    desc.colorGrading.tint = toneMap.tint;
    desc.colorGrading.saturation = toneMap.saturation;
    desc.colorGrading.contrast = toneMap.contrast;
    desc.colorGrading.vignette = toneMap.vignette;
    desc.colorGrading.vignetteFeather = toneMap.vignetteFeather;
    desc.bloom.enabled = bloom.enabled;
    desc.bloom.threshold = bloom.threshold;
    desc.bloom.intensity = bloom.intensity;
    desc.bloom.scatter = bloom.scatter;
    desc.fog.enabled = volFog.enabled;
    desc.fog.density = volFog.density;
    desc.fog.anisotropy = volFog.anisotropy;
    desc.fog.scatterIntensity = volFog.scatterIntensity;
    desc.fog.steps = volFog.steps;
    desc.fog.maxDistance = volFog.maxDistance;
    std::copy(std::begin(volFog.ambientColor), std::end(volFog.ambientColor),
              std::begin(desc.fog.ambientColor));
    desc.fog.ambientIntensity = volFog.ambientIntensity;
    desc.fog.heightFalloff = volFog.heightFalloff;
    desc.fog.heightBase = volFog.heightBase;
    desc.fog.noiseStrength = volFog.noiseStrength;
    desc.fog.noiseScale = volFog.noiseScale;
    desc.fog.noiseSpeed = volFog.noiseSpeed;
    desc.fog.nearClearDistance = volFog.nearClearDistance;
    desc.fog.distanceDensity = volFog.distanceDensity;
}

bool SceneRuntime::AddPointLight(const XMFLOAT3& position)
{
    if (lights.numLights >= 8)
        return false;
    SceneDescriptor::PointLightDesc def;
    def.position = { position.x, position.y, position.z };

    // Keep descs aligned with the bound light slots before appending.
    pointLightDescs.resize(lights.numLights);
    pointLightDescs.push_back(def);

    const int i = lights.numLights++;
    lights.lights[i].position = position;
    lights.lights[i].color = { def.color[0], def.color[1], def.color[2] };
    lights.lights[i].radius = def.radius;
    lights.lights[i].enabled = true;
    lightCastsShadow[i] = def.castShadow;
    return true;
}

SceneRuntime::DetachedPointLight SceneRuntime::RemovePointLight(int index)
{
    DetachedPointLight out;
    if (index < 0 || index >= lights.numLights)
        return out;
    pointLightDescs.resize(lights.numLights);
    out.light      = lights.lights[index];
    out.desc       = pointLightDescs[index];
    out.castShadow = lightCastsShadow[index];
    pointLightDescs.erase(pointLightDescs.begin() + index);
    for (int i = index; i < lights.numLights - 1; ++i)
    {
        lights.lights[i] = lights.lights[i + 1];
        lightCastsShadow[i] = lightCastsShadow[i + 1];
    }
    --lights.numLights;
    lights.lights[lights.numLights] = {};
    lightCastsShadow[lights.numLights] = false;
    return out;
}

bool SceneRuntime::InsertPointLight(int index, const DetachedPointLight& light)
{
    if (lights.numLights >= 8)
        return false;
    index = std::clamp(index, 0, lights.numLights);
    pointLightDescs.resize(lights.numLights);
    pointLightDescs.insert(pointLightDescs.begin() + index, light.desc);
    for (int i = lights.numLights; i > index; --i)
    {
        lights.lights[i] = lights.lights[i - 1];
        lightCastsShadow[i] = lightCastsShadow[i - 1];
    }
    lights.lights[index] = light.light;
    lightCastsShadow[index] = light.castShadow;
    ++lights.numLights;
    return true;
}

bool SceneRuntime::AddParticleEmitter(const XMFLOAT3& position)
{
    if (!m_renderer || !m_shaders)
        return false;
    SceneDescriptor::ParticleEmitterDesc def;
    def.position = { position.x, position.y, position.z };
    def.shaderMode = 1; // procedural ember — visible without an assigned texture

    auto ps = std::make_unique<ParticleSystem>();
    ApplyParticleDescToSystem(def, *ps);
    if (!ps->Init(m_renderer->GetDevice(), *m_shaders))
        return false;
    particleSystems.push_back(std::move(ps));
    particleEmitterDescs.push_back(def);
    selectedParticlePreset.push_back(-1);
    return true;
}

SceneRuntime::DetachedEmitter SceneRuntime::RemoveParticleEmitter(int index)
{
    DetachedEmitter out;
    if (index < 0 || index >= static_cast<int>(particleSystems.size()))
        return out;
    out.system = std::move(particleSystems[index]);
    particleSystems.erase(particleSystems.begin() + index);
    if (index < static_cast<int>(particleEmitterDescs.size()))
    {
        out.desc = std::move(particleEmitterDescs[index]);
        particleEmitterDescs.erase(particleEmitterDescs.begin() + index);
    }
    if (index < static_cast<int>(selectedParticlePreset.size()))
    {
        out.preset = selectedParticlePreset[index];
        selectedParticlePreset.erase(selectedParticlePreset.begin() + index);
    }
    return out;
}

void SceneRuntime::InsertParticleEmitter(int index, DetachedEmitter emitter)
{
    if (!emitter.system)
        return;
    const int count = static_cast<int>(particleSystems.size());
    index = std::clamp(index, 0, count);
    // Keep the parallel arrays aligned with the systems before inserting.
    particleEmitterDescs.resize(count);
    selectedParticlePreset.resize(count, -1);
    particleSystems.insert(particleSystems.begin() + index, std::move(emitter.system));
    particleEmitterDescs.insert(particleEmitterDescs.begin() + index, std::move(emitter.desc));
    selectedParticlePreset.insert(selectedParticlePreset.begin() + index, emitter.preset);
}

bool SceneRuntime::AddSceneObject(SceneDescriptor::SceneObject::Type type,
                                  const XMFLOAT3& position)
{
    if (!m_assets)
        return false;
    LoadedObject lo;
    lo.def.type = type;
    lo.def.position = { position.x, position.y, position.z };
    lo.mat.albedo = m_assets->GetDefaultWhite();
    lo.mat.normal = m_assets->GetDefaultNormal();
    lo.mat.roughness = m_assets->GetDefaultWhite();
    lo.mat.metallic = m_assets->GetDefaultBlack();
    lo.mat.emissive = m_assets->GetDefaultBlack();
    sceneObjects.push_back(std::move(lo));
    return true;
}

SceneRuntime::LoadedObject SceneRuntime::RemoveSceneObject(int index)
{
    if (index < 0 || index >= static_cast<int>(sceneObjects.size()))
        return {};
    LoadedObject out = std::move(sceneObjects[index]);
    sceneObjects.erase(sceneObjects.begin() + index);
    if (m_planarObjectIndex == index)
        m_planarObjectIndex = -1;
    else if (m_planarObjectIndex > index)
        --m_planarObjectIndex;
    return out;
}

void SceneRuntime::InsertSceneObject(int index, LoadedObject object)
{
    index = std::clamp(index, 0, static_cast<int>(sceneObjects.size()));
    if (m_planarObjectIndex >= index)
        ++m_planarObjectIndex;
    // Re-adopt a restored reflector; the planar RT outlives its removal.
    if (object.def.planarReflection && m_planarObjectIndex < 0 &&
        m_planarReflectionRT.GetRTV())
        m_planarObjectIndex = index;
    sceneObjects.insert(sceneObjects.begin() + index, std::move(object));
}

SceneRuntime::LoadedMesh SceneRuntime::RemoveMesh(int index)
{
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size()))
        return {};
    LoadedMesh out = std::move(loadedMeshes[index]);
    loadedMeshes.erase(loadedMeshes.begin() + index);
    return out;
}

void SceneRuntime::InsertMesh(int index, LoadedMesh mesh)
{
    index = std::clamp(index, 0, static_cast<int>(loadedMeshes.size()));
    loadedMeshes.insert(loadedMeshes.begin() + index, std::move(mesh));
}

int SceneRuntime::DuplicatePointLight(int index)
{
    if (index < 0 || index >= lights.numLights || lights.numLights >= 8)
        return -1;
    pointLightDescs.resize(lights.numLights);

    // Descs hold base color/flicker; position and radius are live-edited on
    // the bound slot, so copy those from the source light.
    SceneDescriptor::PointLightDesc def = pointLightDescs[index];
    const auto& source = lights.lights[index];
    def.position = { source.position.x, source.position.y, source.position.z };
    def.radius = source.radius;
    pointLightDescs.push_back(def);

    const int i = lights.numLights++;
    lights.lights[i] = source;
    lights.lights[i].enabled = true;
    lightCastsShadow[i] = lightCastsShadow[index];
    return i;
}

int SceneRuntime::DuplicateParticleEmitter(int index)
{
    if (!m_renderer || !m_shaders ||
        index < 0 || index >= static_cast<int>(particleSystems.size()) ||
        index >= static_cast<int>(particleEmitterDescs.size()))
        return -1;

    SceneDescriptor::ParticleEmitterDesc def = particleEmitterDescs[index];
    SyncParticleDescFromSystem(*particleSystems[index], def);

    auto ps = std::make_unique<ParticleSystem>();
    ApplyParticleDescToSystem(def, *ps);
    if (!ps->Init(m_renderer->GetDevice(), *m_shaders))
        return -1;
    ps->SetTexture(particleSystems[index]->GetTexture());
    particleSystems.push_back(std::move(ps));
    particleEmitterDescs.push_back(def);
    selectedParticlePreset.push_back(
        index < static_cast<int>(selectedParticlePreset.size())
            ? selectedParticlePreset[index] : -1);
    return static_cast<int>(particleSystems.size()) - 1;
}

int SceneRuntime::DuplicateSceneObject(int index)
{
    if (index < 0 || index >= static_cast<int>(sceneObjects.size()))
        return -1;
    LoadedObject copy;
    copy.def = sceneObjects[index].def;
    copy.def.planarReflection = false; // only one authored planar reflector
    copy.mat = sceneObjects[index].mat;
    sceneObjects.push_back(std::move(copy));
    return static_cast<int>(sceneObjects.size()) - 1;
}

int SceneRuntime::DuplicateMesh(int index)
{
    if (index < 0 || index >= static_cast<int>(loadedMeshes.size()))
        return -1;
    const LoadedMesh& source = loadedMeshes[index];
    LoadedMesh copy;
    copy.def = source.def;
    copy.def.sourceEntityID = 0; // a fresh id is assigned on save
    copy.mesh = source.mesh;
    copy.materials = source.materials;
    copy.shadowSubMeshes = source.shadowSubMeshes;

    Entity* entity = world.GetScene().CreateEntity(copy.def.name);
    auto* transform = entity->AddComponent<TransformComponent>();
    if (source.transform)
    {
        transform->position = source.transform->position;
        transform->eulerDeg = source.transform->eulerDeg;
        transform->scale    = source.transform->scale;
    }
    copy.transform = transform;
    loadedMeshes.push_back(std::move(copy));
    return static_cast<int>(loadedMeshes.size()) - 1;
}

int SceneRuntime::AddLoadedMesh(const std::string& path, AssetHandle<Mesh> mesh,
                                std::vector<ForwardPipeline::SubMat> materials,
                                const DirectX::XMFLOAT3& position)
{
    if (!mesh)
        return -1;
    LoadedMesh entry;
    entry.def.path = path;
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.rfind('.');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    entry.def.name = path.substr(start,
        dot != std::string::npos && dot > start ? dot - start : std::string::npos);
    entry.def.position = { position.x, position.y, position.z };
    entry.mesh = std::move(mesh);
    entry.materials = std::move(materials);
    for (auto& sm : entry.materials)
        sm.receiveShadows = entry.def.receiveShadows;
    entry.shadowSubMeshes = BuildShadowSubMeshes(entry.materials);

    Entity* entity = world.GetScene().CreateEntity(entry.def.name);
    auto* transform = entity->AddComponent<TransformComponent>();
    transform->position = position;
    entry.transform = transform;
    loadedMeshes.push_back(std::move(entry));
    return static_cast<int>(loadedMeshes.size()) - 1;
}

void SceneRuntime::RefreshParticlePresets()
{
    particlePresetFiles = SceneLoader::ScanParticlePresetDirectory("Assets/Particles/Presets");
}

void SceneRuntime::ResetBall()
{
    ballTransform->position = ballSpawn;
    ballRigidBody->velocity = { 0.0f, 0.0f, 0.0f };
    m_havePrevBallWorld = false;
}

// --- Per-frame update --------------------------------------------------------

void SceneRuntime::Update(float dt, float totalTime)
{
    ID3D11DeviceContext* ctx = m_renderer->GetContext();

    world.GetScene().Update(dt);
    physicsWorld.Step(dt);
    for (auto& ps : particleSystems)
        ps->Update(ctx, dt);

    UpdatePointLightFlicker(totalTime);

    if (castRay)
    {
        Ray ray;
        XMVECTOR eye = XMLoadFloat3(&camera->eye);
        XMVECTOR dir = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&camera->target), eye));
        XMStoreFloat3(&ray.origin,    eye);
        XMStoreFloat3(&ray.direction, dir);
        rayHitValid = physicsWorld.Raycast(ray, rayHit);
    }
    else
    {
        rayHitValid = false;
    }
}

// Scene-driven firelight: modulate live light color from the base descriptor
// color so UI edits target the descriptor and never fight the modulation.
void SceneRuntime::UpdatePointLightFlicker(float totalTime)
{
    for (int i = 0; i < lights.numLights && i < static_cast<int>(pointLightDescs.size()); ++i)
    {
        const auto& source = pointLightDescs[i];
        float factor = 1.0f;
        if (source.flicker)
        {
            const float phase = static_cast<float>(source.flickerSeed) * 1.618034f;
            const float primary = sinf(totalTime * source.flickerFrequency * XM_2PI + phase);
            const float secondary = sinf(totalTime * source.flickerFrequency * 1.713f * XM_2PI + phase * 2.37f);
            factor += source.flickerAmplitude * (primary * 0.65f + secondary * 0.35f);
            factor = (std::max)(0.1f, factor);
        }
        lights.lights[i].color = {
            source.color[0] * factor, source.color[1] * factor, source.color[2] * factor
        };
    }
}

// --- Render ------------------------------------------------------------------

void SceneRuntime::ResetTemporalHistory()
{
    taa.ResetHistory();
    fsr.ResetHistory();
    m_havePrevViewProj = false;
}

void SceneRuntime::Render(float aspect)
{
    ID3D11DeviceContext* ctx = m_renderer->GetContext();

    XMMATRIX view = camera->GetViewMatrix();
    XMMATRIX proj = camera->GetProjectionMatrix(aspect);
    m_cachedProjNoJitter = proj;
    // Sub-pixel jitter for temporal AA: offset clip-space XY after projection.
    m_viewProjNoJitter = view * proj;
    if (!m_havePrevViewProj)
    {
        m_prevViewProjNoJitter = m_viewProjNoJitter;
        m_havePrevViewProj = true;
    }
    ApplyTemporalJitter(proj);
    m_cachedProj = proj;
    m_cachedView = view;

    const bool litView = pipeline.viewShading == ForwardPipeline::ViewShading::Lit;
    if (litView) RenderShadowMaps(ctx);

    ID3D11ShaderResourceView* fallbackEnvironment = m_assets->GetDefaultBlack()->GetSRV();
    ID3D11ShaderResourceView* irradianceSRV = environmentLighting.GetIrradianceSRV();
    ID3D11ShaderResourceView* specularSRV = environmentLighting.GetSpecularSRV();
    ID3D11ShaderResourceView* brdfLutSRV = environmentLighting.GetBRDFLutSRV();
    if (!irradianceSRV) irradianceSRV = fallbackEnvironment;
    if (!specularSRV) specularSRV = fallbackEnvironment;
    if (!brdfLutSRV) brdfLutSRV = fallbackEnvironment;

    if (litView) RenderPlanarReflection(ctx, irradianceSRV, specularSRV, brdfLutSRV);
    RenderForwardPass(ctx, irradianceSRV, specularSRV, brdfLutSRV);
    if (drawDebugHelpers) RenderDebugShapes(ctx);
    if (pipeline.viewShading != ForwardPipeline::ViewShading::Wireframe) RenderParticles(ctx);
}

int SceneRuntime::CountPointShadowCasters() const
{
    int count = 0;
    for (int i = 0; i < 2 && i < lights.numLights; ++i)
        if (lightCastsShadow[i]) count = i + 1; else break;
    return count;
}

void SceneRuntime::RenderShadowMaps(ID3D11DeviceContext* ctx)
{
    // Cascaded shadow pass
    {
        float er = XMConvertToRadians(lights.elevDeg);
        float ar = XMConvertToRadians(lights.azimDeg);
        XMFLOAT3 lightDir = { cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar) };
        shadowMap.Update(lightDir, m_cachedView, m_cachedProjNoJitter,
                         camera->nearZ, camera->farZ);
        for (int c = 0; c < CSM_NUM_CASCADES; ++c)
        {
            shadowMap.BeginCascade(ctx, c);
            for (const auto& loaded : loadedMeshes)
                if (loaded.visible && loaded.def.castShadows && loaded.mesh && loaded.transform)
                    shadowMap.DrawMesh(ctx, *loaded.mesh, loaded.transform->GetWorldMatrix(),
                                      loaded.shadowSubMeshes);
            if (ballVisible)
                shadowMap.DrawSphere(ctx, ballTransform->position, ballRadius);
            // Analytic spheres cast too; planes are horizontal receivers and skipped.
            for (const auto& object : sceneObjects)
                if (object.visible &&
                    object.def.type == SceneDescriptor::SceneObject::Type::Sphere)
                    shadowMap.DrawSphere(ctx,
                        { object.def.position[0], object.def.position[1],
                          object.def.position[2] }, object.def.radius);
            shadowMap.EndCascade(ctx);
        }
    }

    // Point shadow passes
    {
        const int numCasters = CountPointShadowCasters();
        for (int li = 0; li < numCasters; ++li)
        {
            auto& light = lights.lights[li];
            for (int face = 0; face < 6; ++face)
            {
                pointShadowMaps[li].BeginFace(ctx, face, light.position, light.radius);
                for (const auto& loaded : loadedMeshes)
                    if (loaded.visible && loaded.def.castShadows && loaded.mesh && loaded.transform)
                        pointShadowMaps[li].DrawMesh(
                            ctx, *loaded.mesh, loaded.transform->GetWorldMatrix(),
                            loaded.shadowSubMeshes);
                pointShadowMaps[li].EndFace(ctx);
            }
        }
    }

    // Spot light shadow pass
    if (spotLight.enabled)
    {
        spotLight.BeginShadowPass(ctx);
        for (const auto& loaded : loadedMeshes)
            if (loaded.visible && loaded.def.castShadows && loaded.mesh && loaded.transform)
                spotLight.DrawMesh(ctx, *loaded.mesh, loaded.transform->GetWorldMatrix(),
                               loaded.shadowSubMeshes);
        spotLight.EndShadowPass(ctx);
    }
}

// One bounded planar reflection pass for authored horizontal reflectors.
void SceneRuntime::RenderPlanarReflection(ID3D11DeviceContext* ctx,
                                          ID3D11ShaderResourceView* irradianceSRV,
                                          ID3D11ShaderResourceView* specularSRV,
                                          ID3D11ShaderResourceView* brdfLutSRV)
{
    if (m_planarObjectIndex < 0 || !m_planarReflectionRT.GetRTV())
        return;

    XMFLOAT3 reflectedEye = camera->eye;
    XMFLOAT3 reflectedTarget = camera->target;
    XMFLOAT3 reflectedUp = camera->up;
    reflectedEye.y = 2.0f * m_planarReflectionY - reflectedEye.y;
    reflectedTarget.y = 2.0f * m_planarReflectionY - reflectedTarget.y;
    reflectedUp.y = -reflectedUp.y;
    XMMATRIX reflectedView = XMMatrixLookAtLH(
        XMLoadFloat3(&reflectedEye), XMLoadFloat3(&reflectedTarget), XMLoadFloat3(&reflectedUp));
    m_planarReflectionViewProj = reflectedView * m_cachedProj;

    const float reflectionClear[4] = { 0, 0, 0, 1 };
    m_planarReflectionRT.Begin(ctx, reflectionClear);
    skybox.Draw(ctx, reflectedView, m_cachedProj);
    shadowMap.BindForLitPass(ctx);
    lights.BindPS(ctx, reflectedEye, shadowMap.GetLightViewProj());
    pipeline.Begin(ctx, reflectedView, m_cachedProj);
    pipeline.BindEnvironment(ctx, irradianceSRV, specularSRV, brdfLutSRV);

    const int reflectedShadowCount = CountPointShadowCasters();
    ID3D11ShaderResourceView* reflectedShadowSRVs[2] = { nullptr, nullptr };
    for (int i = 0; i < reflectedShadowCount; ++i)
        reflectedShadowSRVs[i] = pointShadowMaps[i].GetSRV();
    pipeline.BindPointShadows(ctx, reflectedShadowSRVs[0], reflectedShadowSRVs[1],
                              reflectedShadowCount, pointShadowBias);
    spotLight.BindForLitPass(ctx);
    pipeline.SetReflectionPass(ctx, true, m_planarReflectionY);

    for (const auto& loaded : loadedMeshes)
        if (loaded.visible && loaded.mesh && loaded.transform)
        {
            const XMMATRIX worldMatrix = loaded.transform->GetWorldMatrix();
            const XMMATRIX prev = loaded.prevWorldValid
                ? XMLoadFloat4x4(&loaded.prevWorld) : worldMatrix;
            pipeline.SubmitMesh(*loaded.mesh, worldMatrix, loaded.materials, false, &prev);
        }
    if (!loadedMeshes.empty()) pipeline.Flush(ctx);
    if (ballVisible)
    {
        const XMMATRIX ballModel =
            XMMatrixScaling(ballRadius, ballRadius, ballRadius) *
            XMMatrixTranslation(ballTransform->position.x, ballTransform->position.y,
                                ballTransform->position.z);
        const XMMATRIX ballPrev = m_havePrevBallWorld
            ? XMLoadFloat4x4(&m_prevBallWorld) : ballModel;
        pipeline.DrawSphere(ctx, ballTransform->position, ballRadius,
                            { 1.0f, 0.45f, 0.05f }, &ballPrev);
    }
    for (size_t objectIndex = 0; objectIndex < sceneObjects.size(); ++objectIndex)
    {
        if (static_cast<int>(objectIndex) == m_planarObjectIndex) continue;
        const auto& object = sceneObjects[objectIndex];
        if (!object.visible) continue;
        XMFLOAT3 pos = { object.def.position[0], object.def.position[1], object.def.position[2] };
        XMFLOAT3 tint = { object.def.tint[0], object.def.tint[1], object.def.tint[2] };
        XMFLOAT3 emissive = { object.def.emissiveColor[0], object.def.emissiveColor[1], object.def.emissiveColor[2] };
        const XMMATRIX objectModel = SceneObjectModel(object.def);
        const XMMATRIX objectPrev = object.prevWorldValid
            ? XMLoadFloat4x4(&object.prevWorld) : objectModel;
        if (object.def.type == SceneDescriptor::SceneObject::Type::Sphere)
            pipeline.DrawPBRSphere(ctx, pos, object.def.radius, object.mat,
                                   object.def.metallicScale, object.def.roughnessScale,
                                   tint, object.def.emissiveIntensity, emissive,
                                   &objectPrev);
        else
            pipeline.DrawPBRPlane(ctx, pos, object.def.halfSizeX, object.def.halfSizeZ,
                                  object.mat, object.def.metallicScale,
                                  object.def.roughnessScale, tint,
                                  object.def.emissiveIntensity, emissive,
                                  &objectPrev);
    }
    pipeline.SetReflectionPass(ctx, false);
    shadowMap.Unbind(ctx);
    ID3D11ShaderResourceView* nullPointShadows[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(5, 2, nullPointShadows);
    m_planarReflectionRT.End(ctx);
    m_planarReflectionRT.GenerateMips(ctx);
}

// Forward path (HDR + PBR + IBL). Leaves the MRT bound for debug draws.
void SceneRuntime::RenderForwardPass(ID3D11DeviceContext* ctx,
                                     ID3D11ShaderResourceView* irradianceSRV,
                                     ID3D11ShaderResourceView* specularSRV,
                                     ID3D11ShaderResourceView* brdfLutSRV)
{
    {
        ID3D11RenderTargetView* rtvs[3] = { m_forwardHDR_RT.GetRTV(), m_normalRT.GetRTV(),
                                            m_velocityRT.GetRTV() };
        ID3D11DepthStencilView* dsv = m_forwardHDR_RT.GetDSV();
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float clearNorm[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        const float clearVel[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->OMSetRenderTargets(3, rtvs, dsv);
        ctx->ClearRenderTargetView(rtvs[0], black);
        ctx->ClearRenderTargetView(rtvs[1], clearNorm);
        ctx->ClearRenderTargetView(rtvs[2], clearVel);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT vp = {};
        vp.Width = (float)m_forwardHDR_RT.GetWidth();
        vp.Height = (float)m_forwardHDR_RT.GetHeight();
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }

    if (pipeline.viewShading != ForwardPipeline::ViewShading::Wireframe)
        skybox.Draw(ctx, m_cachedView, m_cachedProj);

    // Collect point shadow SRVs
    const int numPtShadows = CountPointShadowCasters();
    ID3D11ShaderResourceView* ptSRVs[2] = { nullptr, nullptr };
    for (int i = 0; i < numPtShadows; ++i)
        ptSRVs[i] = pointShadowMaps[i].GetSRV();

    shadowMap.BindForLitPass(ctx);
    lights.BindPS(ctx, camera->eye, shadowMap.GetLightViewProj());
    pipeline.Begin(ctx, m_cachedView, m_cachedProj);
    pipeline.SetTemporalMatrices(m_prevViewProjNoJitter, m_viewProjNoJitter);
    pipeline.BindEnvironment(ctx, irradianceSRV, specularSRV, brdfLutSRV);
    pipeline.BindPointShadows(ctx, ptSRVs[0], ptSRVs[1], numPtShadows, pointShadowBias);
    spotLight.BindForLitPass(ctx);
    pipeline.SetGlobalMaterialParams(
        { matTint[0], matTint[1], matTint[2] }, roughnessScale, metallic,
        debugShadow ? 1.0f : 0.0f);
    for (auto& loaded : loadedMeshes)
    {
        if (!loaded.mesh || !loaded.transform) continue;
        const XMMATRIX worldMatrix = loaded.transform->GetWorldMatrix();
        const XMMATRIX prev = loaded.prevWorldValid
            ? XMLoadFloat4x4(&loaded.prevWorld) : worldMatrix;
        if (loaded.visible)
            pipeline.SubmitMesh(*loaded.mesh, worldMatrix, loaded.materials, false, &prev);
        XMStoreFloat4x4(&loaded.prevWorld, worldMatrix);
        loaded.prevWorldValid = true;
    }
    if (!loadedMeshes.empty())
        pipeline.Flush(ctx);
    {
        const XMMATRIX ballModel =
            XMMatrixScaling(ballRadius, ballRadius, ballRadius) *
            XMMatrixTranslation(ballTransform->position.x, ballTransform->position.y,
                                ballTransform->position.z);
        const XMMATRIX ballPrev = m_havePrevBallWorld
            ? XMLoadFloat4x4(&m_prevBallWorld) : ballModel;
        if (ballVisible)
            pipeline.DrawSphere(ctx, ballTransform->position, ballRadius,
                                { 1.0f, 0.45f, 0.05f }, &ballPrev);
        XMStoreFloat4x4(&m_prevBallWorld, ballModel);
        m_havePrevBallWorld = true;
    }

    // JSON-driven scene objects — texture maps drive all PBR values (scalars = 1.0)
    for (size_t objectIndex = 0; objectIndex < sceneObjects.size(); ++objectIndex)
    {
        auto& lo = sceneObjects[objectIndex];
        using Type = SceneDescriptor::SceneObject::Type;
        XMFLOAT3 pos  = { lo.def.position[0], lo.def.position[1], lo.def.position[2] };
        XMFLOAT3 tint = { lo.def.tint[0],     lo.def.tint[1],     lo.def.tint[2] };
        XMFLOAT3 eCol = { lo.def.emissiveColor[0], lo.def.emissiveColor[1], lo.def.emissiveColor[2] };
        const XMMATRIX objectModel = SceneObjectModel(lo.def);
        const XMMATRIX objectPrev = lo.prevWorldValid
            ? XMLoadFloat4x4(&lo.prevWorld) : objectModel;
        if (lo.visible)
        {
            if (lo.def.type == Type::Sphere)
                pipeline.DrawPBRSphere(ctx, pos, lo.def.radius, lo.mat,
                                       lo.def.metallicScale, lo.def.roughnessScale,
                                       tint, lo.def.emissiveIntensity, eCol,
                                       &objectPrev);
            else
            {
                if (static_cast<int>(objectIndex) == m_planarObjectIndex)
                    pipeline.BindPlanarReflection(ctx, m_planarReflectionRT.GetSRV(),
                                                  m_planarReflectionViewProj,
                                                  m_planarReflectionY,
                                                  m_planarReflectionStrength);
                pipeline.DrawPBRPlane(ctx, pos, lo.def.halfSizeX, lo.def.halfSizeZ,
                                      lo.mat, lo.def.metallicScale, lo.def.roughnessScale,
                                      tint, lo.def.emissiveIntensity, eCol,
                                      &objectPrev);
                if (static_cast<int>(objectIndex) == m_planarObjectIndex)
                    pipeline.ClearPlanarReflection(ctx);
            }
        }
        XMStoreFloat4x4(&lo.prevWorld, objectModel);
        lo.prevWorldValid = true;
    }

    shadowMap.Unbind(ctx);

    // Unbind point shadow SRVs
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(5, 2, nullSRVs);
    ctx->RSSetState(nullptr); // do not leak viewport wireframe into fullscreen post-processing
}

void SceneRuntime::RenderDebugShapes(ID3D11DeviceContext* ctx)
{
    if (showColliders)
    {
        pipeline.DrawWireSphere(ctx, ballTransform->position, ballRadius,
                                { 0.0f, 1.0f, 0.2f });
        pipeline.DrawWireBox(ctx, obbFloor.GetWorldMatrix(), { 1.0f, 0.85f, 0.1f });

        XMFLOAT3 bottom = { cc.position.x, cc.position.y + cc.radius, cc.position.z };
        pipeline.DrawWireSphere(ctx, bottom, cc.radius, { 1.0f, 0.4f, 0.8f });
        {
            XMFLOAT3 col = cc.isGrounded ? XMFLOAT3{ 1.0f, 0.0f, 1.0f }
                                         : XMFLOAT3{ 0.25f, 0.0f, 0.25f };
            XMFLOAT3 tip = { bottom.x + cc.contactNormal.x,
                             bottom.y + cc.contactNormal.y,
                             bottom.z + cc.contactNormal.z };
            pipeline.DrawLine(ctx, bottom, tip, col);
        }
    }

    if (rayHitValid)
    {
        pipeline.DrawWireSphere(ctx, rayHit.point, 0.15f, { 1.0f, 1.0f, 1.0f });
        pipeline.DrawLine(ctx, rayHit.point,
            { rayHit.point.x + rayHit.normal.x,
              rayHit.point.y + rayHit.normal.y,
              rayHit.point.z + rayHit.normal.z },
            { 1.0f, 1.0f, 0.0f });
    }

    // Spot light icon (small sphere + direction line)
    if (spotLight.enabled)
    {
        pipeline.DrawWireSphere(ctx, spotLight.position, 0.3f, spotLight.color);
        XMFLOAT3 tip = {
            spotLight.position.x + spotLight.direction.x * 2.0f,
            spotLight.position.y + spotLight.direction.y * 2.0f,
            spotLight.position.z + spotLight.direction.z * 2.0f
        };
        pipeline.DrawLine(ctx, spotLight.position, tip, spotLight.color);
    }
}

// Particles — render into HDR RT with depth read (no write).
// Use read-only DSV so depth SRV can be bound simultaneously for soft particles.
void SceneRuntime::RenderParticles(ID3D11DeviceContext* ctx)
{
    if (particleSystems.empty())
        return;

    ID3D11RenderTargetView* rtv = m_forwardHDR_RT.GetRTV();
    auto* readOnlyDSV = m_forwardHDR_RT.GetReadOnlyDSV();
    auto* dsv = readOnlyDSV ? readOnlyDSV : m_forwardHDR_RT.GetDSV();
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    for (auto& ps : particleSystems)
    {
        const float er = XMConvertToRadians(lights.elevDeg);
        const float ar = XMConvertToRadians(lights.azimDeg);
        XMFLOAT3 particleLightDir = {
            cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar)
        };
        XMFLOAT3 particleLightColor = {
            lights.lightColor[0] * lights.lightIntensity,
            lights.lightColor[1] * lights.lightIntensity,
            lights.lightColor[2] * lights.lightIntensity
        };
        ps->Render(ctx, m_cachedView, m_cachedProj,
            m_forwardHDR_RT.GetWidth(), m_forwardHDR_RT.GetHeight(),
            m_forwardHDR_RT.GetDepthSRV(),
            camera->nearZ, camera->farZ,
            particleLightDir, particleLightColor,
            0.18f * lights.iblIntensity);
    }
}

// --- Post-process ------------------------------------------------------------

void SceneRuntime::PostProcess(uint32_t outputWidth, uint32_t outputHeight, float frameDt,
                               const std::function<void()>& bindOutput)
{
    ID3D11DeviceContext* ctx = m_renderer->GetContext();
    m_outputWidth  = outputWidth;
    m_outputHeight = outputHeight;

    const uint32_t w = outputWidth, h = outputHeight;

    // FSR renders the scene at reduced resolution and upscales to the output.
    const float fsrRatio = fsrEnabled && !camera->orthographic ? FsrUpscaleRatio() : 1.0f;
    const uint32_t rw = (std::max)(1u, static_cast<uint32_t>(w / fsrRatio));
    const uint32_t rh = (std::max)(1u, static_cast<uint32_t>(h / fsrRatio));

    // Resize render targets when the output or the render resolution changes.
    if (m_forwardHDR_RT.GetWidth() != rw || m_forwardHDR_RT.GetHeight() != rh ||
        m_ldrRT.GetWidth() != w || m_ldrRT.GetHeight() != h)
        ResizeRenderTargets(rw, rh, w, h);

    RenderTarget& hdrRT = m_forwardHDR_RT;

    // Unbind MRT — only need single RTV for post-process
    {
        ID3D11RenderTargetView* nullRTV[3] = { nullptr, nullptr, nullptr };
        ctx->OMSetRenderTargets(3, nullRTV, nullptr);
    }

    const bool litView = pipeline.viewShading == ForwardPipeline::ViewShading::Lit;
    // These effects currently reconstruct perspective rays. Utility ortho
    // views omit them rather than applying an incorrect depth interpretation.
    if (ssao.enabled && litView && !camera->orthographic)
        ssao.Apply(ctx, hdrRT, hdrRT.GetDepthSRV(), m_normalRT.GetSRV(), m_cachedProj);

    // Volumetric fog / god rays (in-scatter into hdrRT before bloom picks up shafts)
    if (volFog.enabled && litView && !camera->orthographic)
    {
        float er = XMConvertToRadians(lights.elevDeg);
        float ar = XMConvertToRadians(lights.azimDeg);
        XMFLOAT3 lightDir = { cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar) };
        XMFLOAT3 lightColor = { lights.lightColor[0] * lights.lightIntensity,
                                lights.lightColor[1] * lights.lightIntensity,
                                lights.lightColor[2] * lights.lightIntensity };
        XMMATRIX cascadeVP[3] = { shadowMap.GetCascadeVP(0),
                                  shadowMap.GetCascadeVP(1),
                                  shadowMap.GetCascadeVP(2) };
        float splitFar[2] = { shadowMap.GetSplitFar(0), shadowMap.GetSplitFar(1) };
        XMFLOAT3 spotColor = { spotLight.color.x * spotLight.intensity,
                               spotLight.color.y * spotLight.intensity,
                               spotLight.color.z * spotLight.intensity };
        volFog.Apply(ctx, hdrRT, hdrRT.GetDepthSRV(),
                     shadowMap.GetShadowSRV(), shadowMap.GetShadowSampler(),
                     iblTexture ? iblTexture->GetSRV() : skybox.GetPanoramaSRV(),
                     spotLight.GetSRV(),
                     m_cachedView, m_cachedProj, camera->eye,
                     lightDir, lightColor,
                     spotLight.position, spotLight.direction, spotColor,
                     spotLight.range,
                     cosf(XMConvertToRadians(spotLight.innerAngle)),
                     cosf(XMConvertToRadians(spotLight.outerAngle)),
                     spotLight.shadowBias, spotLight.volumetricIntensity,
                     spotLight.enabled ? 1 : 0, spotLight.GetViewProj(),
                     cascadeVP, splitFar, frameDt);
    }

    UpdateFsrLifetime(rw, rh, w, h);

    // Upscale (FSR) or TAA before bloom so bloom receives a temporally
    // stable image at output resolution.
    const bool useFsr = fsrEnabled && !camera->orthographic && fsr.IsInitialized() &&
                        hdrRT.GetWidth() == fsr.RenderWidth() &&
                        hdrRT.GetHeight() == fsr.RenderHeight();
    if (useFsr)
    {
        DispatchFsr(ctx, frameDt);
    }
    else if (taa.enabled)
    {
        XMMATRIX invVP = XMMatrixInverse(nullptr, m_viewProjNoJitter);
        taa.Apply(ctx, hdrRT, hdrRT.GetDepthSRV(), m_velocityRT.GetSRV(),
                  invVP, m_prevViewProjNoJitter);
    }
    m_prevViewProjNoJitter = m_viewProjNoJitter;

    RenderTarget& outRT = useFsr ? m_fsrOutputRT : hdrRT;

    if (bloom.enabled && litView)
        bloom.Apply(ctx, outRT);

    if (fxaa.enabled)
    {
        // Tone map into intermediate LDR RT, then FXAA into the output
        float clearBlack[4] = { 0, 0, 0, 1 };
        m_ldrRT.Begin(ctx, clearBlack);
        toneMap.Apply(ctx, outRT.GetSRV(), w, h);
        m_ldrRT.End(ctx);

        bindOutput();
        fxaa.Apply(ctx, m_ldrRT.GetSRV(), w, h);
    }
    else
    {
        bindOutput();
        toneMap.Apply(ctx, outRT.GetSRV(), w, h);
    }
}

float SceneRuntime::FsrUpscaleRatio() const
{
    switch (fsrQuality)
    {
    case 1:  return 1.7f;  // Balanced
    case 2:  return 2.0f;  // Performance
    case 3:  return 1.0f;  // Native AA
    default: return 1.5f;  // Quality
    }
}

// (Re)create scene render targets at render resolution (rw, rh) and
// display-resolution targets at (w, h).
void SceneRuntime::ResizeRenderTargets(uint32_t rw, uint32_t rh, uint32_t w, uint32_t h)
{
    ID3D11Device* dev = m_renderer->GetDevice();
    m_forwardHDR_RT.Shutdown();
    m_forwardHDR_RT.Init(dev, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true);
    m_normalRT.Shutdown();
    m_normalRT.Init(dev, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_velocityRT.Shutdown();
    m_velocityRT.Init(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT);
    taa.Resize(dev, rw, rh);
    bloom.Resize(dev, w, h);
    ssao.Resize(dev, rw, rh);
    volFog.Resize(dev, rw, rh);
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

// Create/destroy the FSR context when its configuration changes.
void SceneRuntime::UpdateFsrLifetime(uint32_t rw, uint32_t rh, uint32_t w, uint32_t h)
{
    if (!fsrEnabled || camera->orthographic)
    {
        if (fsr.IsInitialized())
        {
            fsr.Shutdown();
            m_fsrOutputRT.Shutdown();
        }
        return;
    }
    if (fsr.IsInitialized() &&
        fsr.RenderWidth() == rw && fsr.RenderHeight() == rh &&
        m_fsrOutputRT.GetWidth() == w && m_fsrOutputRT.GetHeight() == h)
        return;

    ID3D11Device* dev = m_renderer->GetDevice();
    if (!fsr.Init(dev, rw, rh, w, h))
        fsrEnabled = false;
    fsr.SetVelocityFactor(fsrVelocityFactor);
    m_fsrOutputRT.Shutdown();
    m_fsrOutputRT.Init(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                       false, false, false, /*withUAV=*/true);
}

void SceneRuntime::DispatchFsr(ID3D11DeviceContext* ctx, float frameDt)
{
    // The HDR RT may still be bound as RTV from the previous pass; FSR binds
    // it as a compute SRV.
    ID3D11RenderTargetView* nullRTV[3] = { nullptr, nullptr, nullptr };
    ctx->OMSetRenderTargets(3, nullRTV, nullptr);

    FSRUpscaler::DispatchParams p;
    p.color            = m_forwardHDR_RT.GetTexture();
    p.depth            = m_forwardHDR_RT.GetDepthTexture();
    p.motionVectors    = m_velocityRT.GetTexture();
    p.output           = m_fsrOutputRT.GetTexture();
    p.frameTimeSeconds = frameDt;
    p.cameraNear       = camera->nearZ;
    p.cameraFar        = camera->farZ;
    p.cameraFovY       = XMConvertToRadians(camera->fovDeg);
    p.sharpen          = fsrSharpen;
    p.sharpness        = fsrSharpness;
    fsr.Dispatch(ctx, p);
}

// Sub-pixel projection jitter for FSR (its own sequence) or TAA.
void SceneRuntime::ApplyTemporalJitter(XMMATRIX& proj)
{
    if (fsrEnabled && !camera->orthographic && fsr.IsInitialized())
    {
        const XMFLOAT2 jitter = fsr.NextJitter();
        proj = proj * XMMatrixTranslation(jitter.x, jitter.y, 0.0f);
    }
    else if (taa.enabled)
    {
        const XMFLOAT2 jitter =
            taa.NextJitter(m_forwardHDR_RT.GetWidth(), m_forwardHDR_RT.GetHeight());
        proj = proj * XMMatrixTranslation(jitter.x, jitter.y, 0.0f);
    }
}

} // namespace SE
