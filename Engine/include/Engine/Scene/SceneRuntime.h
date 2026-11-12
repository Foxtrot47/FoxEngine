#pragma once
#include <windows.h>
#include <DirectXMath.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Assets/AssetManager.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Physics/OBB.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/RigidBodyComponent.h"
#include "Engine/Renderer/Bloom.h"
#include "Engine/Renderer/CascadedShadowMap.h"
#include "Engine/Renderer/EnvironmentLighting.h"
#include "Engine/Renderer/FSRUpscaler.h"
#include "Engine/Renderer/FXAA.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Renderer/LightEnvironment.h"
#include "Engine/Renderer/ParticleSystem.h"
#include "Engine/Renderer/PointShadowMap.h"
#include "Engine/Renderer/ShadowMaterial.h"
#include "Engine/Renderer/RenderTarget.h"
#include "Engine/Renderer/SSAO.h"
#include "Engine/Renderer/SkyboxRenderer.h"
#include "Engine/Renderer/SpotLight.h"
#include "Engine/Renderer/TAA.h"
#include "Engine/Renderer/ToneMap.h"
#include "Engine/Renderer/VolumetricFog.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Scene/Camera/CameraController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/SceneDescriptor.h"
#include "Engine/Scene/TransformComponent.h"

namespace SE {

class Renderer;
class ShaderLibrary;
class InputManager;

// Owns a fully loaded level and everything needed to simulate and render it:
// forward pipeline, lights + shadow maps, post-process chain, physics, and
// particles. Levels load asynchronously and commit transactionally. The final
// tonemapped image lands wherever the caller binds in PostProcess, so the same
// runtime drives the game back buffer or an editor viewport render target.
//
// Tunable state is public in the style of the individual effect classes
// (ToneMap, Bloom, ...) so tooling UI can edit it directly.
class SceneRuntime
{
public:
    struct LoadedMesh
    {
        SceneDescriptor::MeshEntry           def;
        AssetHandle<Mesh>                    mesh;
        std::vector<ForwardPipeline::SubMat> materials;
        TransformComponent*                  transform = nullptr;
        bool                                 visible   = true;
        std::vector<ShadowSubMesh>           shadowSubMeshes;
        DirectX::XMFLOAT4X4                  prevWorld = {};
        bool                                 prevWorldValid = false;
        std::shared_ptr<const StaticMeshCollider> collider; // set when def.collision
    };

    struct LoadedObject
    {
        SceneDescriptor::SceneObject def;
        ForwardPipeline::SubMat      mat;
        bool                         visible = true;
        DirectX::XMFLOAT4X4          prevWorld = {};
        bool                         prevWorldValid = false;
    };

    ~SceneRuntime();

    bool Init(Renderer& renderer, ShaderLibrary& shaders, AssetManager& assets,
              InputManager& input, HWND hwnd, uint32_t width, uint32_t height);

    // Joins the async load worker. Call before the D3D device is released.
    void Shutdown();

    // --- Async scene loading ---
    void RequestSceneLoad(const std::string& scenePath);
    // Advances the load state machine; call once per frame before Update().
    // pumpMessages keeps the window responsive while the scene is applied;
    // return false from it to abort the apply. Returns true on the frame the
    // newly applied scene becomes active.
    bool TickSceneLoad(const std::function<bool()>& pumpMessages);
    bool IsLoading() const { return !m_loadingScenePath.empty(); }
    bool HasScene() const { return camera != nullptr; }
    LevelLoadStatus GetLoadStatus() const { return m_levelLoad.GetStatus(); }
    const std::string& GetLoadingScenePath() const { return m_loadingScenePath; }

    // --- Per-frame (only when HasScene() and not loading) ---
    void Update(float dt, float totalTime);
    // Shadow passes + forward PBR pass + particles into the internal HDR RT.
    void Render(float aspect);
    // SSAO -> fog -> FSR/TAA -> bloom -> tonemap (-> FXAA). bindOutput must
    // bind the final destination RTV (back buffer or an offscreen target).
    void PostProcess(uint32_t outputWidth, uint32_t outputHeight, float frameDt,
                     const std::function<void()>& bindOutput);

    // Jittered matrices used for this frame's scene rendering.
    DirectX::XMMATRIX GetView() const { return m_cachedView; }
    DirectX::XMMATRIX GetProj() const { return m_cachedProj; }

    void ResetTemporalHistory();
    void RefreshParticlePresets();
    static void ApplyParticleDescToSystem(const SceneDescriptor::ParticleEmitterDesc& d,
                                          ParticleSystem& ps);
    static void SyncParticleDescFromSystem(const ParticleSystem& ps,
                                           SceneDescriptor::ParticleEmitterDesc& d);

    // Stamps live tuning state (lights, post-process, physics, entity defs)
    // back into currentDesc so the edited scene can be serialized.
    void SyncDescriptorFromRuntime();

    // Entity state detached by Remove*. Insert* puts it back at the same index
    // so editor undo can restore removed entities (GPU resources included).
    struct DetachedPointLight
    {
        LightEnvironment::PointLight    light;
        SceneDescriptor::PointLightDesc desc;
        bool                            castShadow = false;
    };
    struct DetachedEmitter
    {
        std::unique_ptr<ParticleSystem>      system;
        SceneDescriptor::ParticleEmitterDesc desc;
        int                                  preset = -1;
    };

    // Runtime entity add/remove (editor tooling). Add* returns false when the
    // entity cannot be created (light capacity, missing device). Remove*
    // returns an empty state for out-of-range indices; Insert* clamps the
    // index to the current count.
    bool AddPointLight(const DirectX::XMFLOAT3& position);
    DetachedPointLight RemovePointLight(int index);
    bool InsertPointLight(int index, const DetachedPointLight& light);
    bool AddParticleEmitter(const DirectX::XMFLOAT3& position);
    DetachedEmitter RemoveParticleEmitter(int index);
    void InsertParticleEmitter(int index, DetachedEmitter emitter);
    bool AddSceneObject(SceneDescriptor::SceneObject::Type type,
                        const DirectX::XMFLOAT3& position);
    LoadedObject RemoveSceneObject(int index);
    void InsertSceneObject(int index, LoadedObject object);
    // The mesh's world entity is kept alive, so the returned transform stays
    // valid for a later InsertMesh.
    LoadedMesh RemoveMesh(int index);
    void InsertMesh(int index, LoadedMesh mesh);

    // Appends a copy of the entity at index (same live state, new slot) and
    // returns the new index, or -1 on failure/capacity.
    int DuplicatePointLight(int index);
    int DuplicateParticleEmitter(int index);
    int DuplicateSceneObject(int index);
    int DuplicateMesh(int index);
    // Adds an already-uploaded mesh (see MeshAddOperation) as a new entity.
    int AddLoadedMesh(const std::string& path, AssetHandle<Mesh> mesh,
                      std::vector<ForwardPipeline::SubMat> materials,
                      const DirectX::XMFLOAT3& position);

    // --- Physics balls (spawned on demand, e.g. by scripts; not saved) ---
    struct PhysicsBall
    {
        EntityID            entity    = 0;
        TransformComponent* transform = nullptr;
        RigidBodyComponent* body      = nullptr; // mass/friction/velocity edits reach the simulation
        BodyHandle          handle    = k_invalidBody;
        float               radius    = 0.5f;
        // Previous model matrix for per-object motion vectors (TAA).
        DirectX::XMFLOAT4X4 prevWorld = {};
        bool                prevWorldValid = false;
    };
    // Adds a dynamic sphere; it lives until RemoveBalls() or the next scene
    // load. Returns the ball's body, or k_invalidBody on failure.
    BodyHandle SpawnBall(const DirectX::XMFLOAT3& position, float radius,
                         const DirectX::XMFLOAT3& velocity = {});
    void RemoveBalls();

    // --- Rendering subsystems ---
    SkyboxRenderer         skybox;
    AssetHandle<Texture2D> environmentTexture;
    AssetHandle<Texture2D> iblTexture;
    EnvironmentLighting    environmentLighting;
    LightEnvironment       lights;
    ForwardPipeline        pipeline;
    CascadedShadowMap      shadowMap;
    PointShadowMap         pointShadowMaps[2];
    SpotLight              spotLight;
    ToneMap                toneMap;
    Bloom                  bloom;
    SSAO                   ssao;
    VolumetricFog          volFog;
    FXAA                   fxaa;
    TAA                    taa;
    FSRUpscaler            fsr;
    bool                   fsrEnabled        = true;
    int                    fsrQuality        = 3; // 0=Quality 1=Balanced 2=Performance 3=Native AA
    bool                   fsrSharpen        = true;
    float                  fsrSharpness      = 0.4f;
    float                  fsrVelocityFactor = 0.0f; // 0 = stable thin features

    // Global material overrides (scale texture-driven values)
    float matTint[3]     = { 1.0f, 1.0f, 1.0f };
    float roughnessScale = 1.0f;
    float metallic       = 1.0f;

    // --- Scene state ---
    LevelWorld       world;
    CameraComponent* camera = nullptr;
    CameraController camCtrl;

    std::vector<LoadedMesh>                        loadedMeshes;
    std::vector<LoadedObject>                      sceneObjects;
    std::vector<std::unique_ptr<ParticleSystem>>   particleSystems;
    std::vector<SceneDescriptor::ParticleEmitterDesc> particleEmitterDescs;
    std::vector<std::string>                       particlePresetFiles;
    std::vector<int>                               selectedParticlePreset;
    std::vector<SceneDescriptor::PointLightDesc>   pointLightDescs;
    bool                                           lightCastsShadow[8] = { true };
    float                                          pointShadowBias     = 0.015f;
    std::vector<LevelDiagnostic>                   levelDiagnostics;
    SceneDescriptor                                currentDesc;
    std::string                                    currentScenePath;
    std::string                                    lastRequestedScenePath; // for retry after a failed load
    std::string                                    sceneLoadError;

    // --- Physics ---
    PhysicsWorld            physicsWorld;
    BodyHandle              floorBody     = k_invalidBody;
    float                   floorY        = 0.0f;
    OBB                     obbFloor;
    CharacterController     cc;
    bool                    gravityEnabled = true;
    PhysicsWorld::RaycastHit rayHit      = {};
    bool                    rayHitValid  = false;
    // Declared after physicsWorld and world: balls are destroyed first.
    std::vector<PhysicsBall> balls;

    // --- Debug toggles ---
    bool showColliders = false;
    bool drawDebugHelpers = true; // viewport-only; never serialized
    bool castRay       = false;
    bool debugShadow   = false;

private:
    bool ApplyScene(const std::string& scenePath, PreparedLevel&& prepared,
                    const std::function<bool()>& pumpMessages);
    bool ImportSceneMeshes(const SceneDescriptor& desc, PreparedLevel& prepared,
                           const std::function<bool()>& pumpMessages,
                           std::vector<LoadedMesh>& outMeshes);
    bool CreateParticleSystems(const SceneDescriptor& desc, PreparedLevel& prepared,
                               const std::function<bool()>& pumpMessages,
                               std::vector<std::unique_ptr<ParticleSystem>>& outSystems,
                               std::vector<SceneDescriptor::ParticleEmitterDesc>& outDescs);
    std::vector<int> MatchParticlePresets(
        const std::vector<SceneDescriptor::ParticleEmitterDesc>& descs) const;
    void SetupPlanarReflection(ID3D11Device* device);
    void ApplySceneSettings(const SceneDescriptor& desc);
    void BeginAsyncSceneLoad(const std::string& scenePath);

    void UpdatePointLightFlicker(float totalTime);

    // One visible level mesh with the data every render phase needs, so the
    // shadow, reflection and lit passes stay in step instead of duplicating
    // the iteration.
    struct MeshInstance
    {
        const Mesh*        mesh              = nullptr;
        DirectX::XMMATRIX  model             = {};
        DirectX::XMMATRIX  prevModel         = {};
        const std::vector<ForwardPipeline::SubMat>* materials = nullptr;
        // Shadow inputs from BuildShadowSubMeshes.
        const std::vector<ShadowSubMesh>*   shadowSubMeshes  = nullptr;
        bool               castsShadows      = true;
    };
    template <typename Fn> void ForEachMeshInstance(Fn&& fn) const;

    // Per-submesh shadow inputs: skips transparent materials and supplies
    // alpha-test maps for cutouts.
    static std::vector<ShadowSubMesh> BuildShadowSubMeshes(
        const std::vector<ForwardPipeline::SubMat>& materials);

    // Render phases
    int  CountPointShadowCasters() const;
    void RenderShadowMaps(ID3D11DeviceContext* ctx);
    void RenderPlanarReflection(ID3D11DeviceContext* ctx,
                                ID3D11ShaderResourceView* irradianceSRV,
                                ID3D11ShaderResourceView* specularSRV,
                                ID3D11ShaderResourceView* brdfLutSRV);
    void RenderForwardPass(ID3D11DeviceContext* ctx,
                           ID3D11ShaderResourceView* irradianceSRV,
                           ID3D11ShaderResourceView* specularSRV,
                           ID3D11ShaderResourceView* brdfLutSRV);
    void SubmitMeshInstances(ID3D11DeviceContext* ctx);
    void DrawBalls(ID3D11DeviceContext* ctx);
    void RecordPrevTransforms();
    void RenderDebugShapes(ID3D11DeviceContext* ctx);
    void RenderParticles(ID3D11DeviceContext* ctx);

    // Post-process helpers
    float FsrUpscaleRatio() const;
    void  ResizeRenderTargets(uint32_t rw, uint32_t rh, uint32_t w, uint32_t h);
    void  UpdateFsrLifetime(uint32_t rw, uint32_t rh, uint32_t w, uint32_t h);
    void  DispatchFsr(ID3D11DeviceContext* ctx, float frameDt);
    void  ApplyTemporalJitter(DirectX::XMMATRIX& proj);

    Renderer*      m_renderer = nullptr;
    ShaderLibrary* m_shaders  = nullptr;
    AssetManager*  m_assets   = nullptr;
    InputManager*  m_input    = nullptr;
    HWND           m_hwnd     = nullptr;

    // Display-resolution output size (window or editor viewport).
    uint32_t m_outputWidth  = 0;
    uint32_t m_outputHeight = 0;

    RenderTarget m_forwardHDR_RT;
    RenderTarget m_normalRT;
    RenderTarget m_velocityRT;
    RenderTarget m_ldrRT;
    RenderTarget m_fsrOutputRT;

    RenderTarget      m_planarReflectionRT;
    int               m_planarObjectIndex        = -1;
    float             m_planarReflectionY        = 0.0f;
    float             m_planarReflectionStrength = 1.0f;
    float             m_planarReflectionScale    = 0.5f;
    DirectX::XMMATRIX m_planarReflectionViewProj = DirectX::XMMatrixIdentity();

    DirectX::XMMATRIX m_viewProjNoJitter     = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_prevViewProjNoJitter = DirectX::XMMatrixIdentity();
    bool              m_havePrevViewProj     = false;
    DirectX::XMMATRIX m_cachedView           = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_cachedProj           = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX m_cachedProjNoJitter   = DirectX::XMMatrixIdentity();

    LevelLoadOperation m_levelLoad;
    std::string        m_pendingSceneLoad;
    std::string        m_loadingScenePath;
    bool               m_completeSceneLoadNextFrame = false;
};

} // namespace SE
