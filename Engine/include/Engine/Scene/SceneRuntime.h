#pragma once
#include <windows.h>
#include <array>
#include <DirectXMath.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Assets/AssetManager.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Physics/OBB.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Physics/RigidBodyComponent.h"
#include "Engine/Physics/Vehicle/Vehicle.h"
#include "Engine/Renderer/Bloom.h"
#include "Engine/Renderer/CascadedShadowMap.h"
#include "Engine/Renderer/EnvironmentLighting.h"
#include "Engine/Renderer/FSRUpscaler.h"
#include "Engine/Renderer/FXAA.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Renderer/LightEnvironment.h"
#include "Engine/Renderer/MotionBlur.h"
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
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Scene/LevelWorld.h"
#include "Engine/Scene/MeshAddOperation.h"
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
        DirectX::XMFLOAT4X4                  colliderWorld = {}; // pose the body was added at
    };

    struct LoadedObject
    {
        SceneDescriptor::SceneObject def;
        ForwardPipeline::SubMat      mat;
        bool                         visible = true;
        DirectX::XMFLOAT4X4          prevWorld = {};
        bool                         prevWorldValid = false;
    };

    // An empty entity: its world entity's transform is the source of truth.
    struct LoadedEmpty
    {
        SceneDescriptor::EmptyEntity def;
        TransformComponent*          transform = nullptr;
    };

    ~SceneRuntime();

    bool Init(Renderer& renderer, ShaderLibrary& shaders, AssetManager& assets,
              InputManager& input, HWND hwnd, uint32_t width, uint32_t height);

    // Joins the async load worker. Call before the D3D device is released.
    void Shutdown();

    // --- Async scene loading ---
    // levelJson, when non-empty, is FoxLevel JSON text loaded instead of
    // reading scenePath from disk; scenePath stays the scene's identity
    // (editor Play mode loads the unsaved editor state this way).
    void RequestSceneLoad(const std::string& scenePath, std::string levelJson = {});
    // Advances the load state machine; call once per frame before Update().
    // pumpMessages keeps the window responsive while the scene is applied;
    // return false from it to abort the apply. Returns true on the frame the
    // newly applied scene becomes active.
    bool TickSceneLoad(const std::function<bool()>& pumpMessages);
    bool IsLoading() const { return !m_loadingScenePath.empty(); }
    bool HasScene() const { return camera != nullptr; }
    LevelLoadStatus GetLoadStatus() const { return m_levelLoad.GetStatus(); }
    const std::string& GetLoadingScenePath() const { return m_loadingScenePath; }
    // Called on the main thread when a newly loaded scene is about to replace
    // the current one: every fallible load step has passed and the outgoing
    // scene is still intact (the ScriptSystem runs on_destroy here).
    std::function<void()> onSceneUnloading;

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
    // Empty entities. Remove keeps the world entity alive (like meshes), so
    // the returned transform stays valid for a later InsertEmpty.
    bool AddEmpty(const DirectX::XMFLOAT3& position);
    LoadedEmpty RemoveEmpty(int index);
    void InsertEmpty(int index, LoadedEmpty empty);
    int DuplicateEmpty(int index);

    // --- Scripted entity state (fox.Entity / fox.Transform) ---
    // Pushes the entity's world position into the runtime state it drives:
    // point light, particle emitter and primitive positions (meshes and
    // empties render from their TransformComponent directly), then recurses
    // into its children. The camera is driven through fox.camera instead.
    void ApplyEntityTransform(const Entity& entity);
    // Sets entity.active and shows/hides what the entity renders (mesh,
    // primitive, point light, particle emitter).
    void SetEntityActive(Entity& entity, bool active);

    // --- Vehicles (spawned on demand, e.g. by scripts; not saved) ---
    struct VehiclePart
    {
        AssetHandle<Mesh>                    mesh;
        std::vector<ForwardPipeline::SubMat> materials;
        std::vector<ShadowSubMesh>           shadowSubMeshes;
    };
    struct VehicleInstance
    {
        uint32_t                 id        = 0;
        EntityID                 entity    = 0;
        std::unique_ptr<Vehicle> vehicle;
        TransformComponent*      transform = nullptr; // vehicle origin, physics-driven
        VehiclePart              body;
        std::array<VehiclePart, VehicleDesc::WheelCount> wheels;   // suspension, steering, spin
        std::array<VehiclePart, VehicleDesc::WheelCount> spindles; // suspension, steering; optional
        // Previous chassis and wheel transforms for per-object motion vectors
        // (TAA). Wheel rotation is part of the matrix, so spinning wheels need
        // their own previous pose instead of reusing this frame's.
        DirectX::XMFLOAT4X4 prevChassisWorld = {};
        std::array<DirectX::XMFLOAT4X4, VehicleDesc::WheelCount> prevWheelLocal = {};
        std::array<DirectX::XMFLOAT4X4, VehicleDesc::WheelCount> prevSpindleLocal = {};
        bool                prevChassisValid = false;
        bool                prevWheelsValid  = false;
    };
    enum class VehicleSpawnState { Idle, Loading, Failed };

    // Starts loading a vehicle definition and its meshes asynchronously; the
    // car appears in `vehicles` (upright, wheels on groundPosition) once its
    // meshes are uploaded. Returns the vehicle id (0 on failure, see
    // GetVehicleSpawnError). One spawn may be in flight at a time.
    uint32_t RequestVehicleSpawn(const std::string& vehiclePath,
                                 const DirectX::XMFLOAT3& groundPosition, float yawDeg);
    VehicleSpawnState  GetVehicleSpawnState() const;
    const std::string& GetVehicleSpawnError() const { return m_vehicleSpawnError; }
    // The ready vehicle with this id, or null (still loading, failed, removed).
    VehicleInstance* FindVehicle(uint32_t id);
    const VehicleInstance* FindVehicle(uint32_t id) const;
    void RemoveVehicles();

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

    // --- Actors (fox.actor placements and fox.world.spawn) ---
    // Every part is a child entity of the actor's root entity, so moving the
    // root (or a part) moves what it renders; kinematic colliders follow
    // their part each frame. A dynamic body instead drives the actor root.
    // A part shows (and has its body) while both the root and the part
    // entity are active; SetEntityActive on either refreshes it.
    struct ActorPart
    {
        ActorComponentDefinition def;
        TransformComponent*      transform = nullptr; // the part's child entity
        // mesh
        AssetHandle<Mesh>                    mesh;
        std::vector<ForwardPipeline::SubMat> materials;
        std::vector<ShadowSubMesh>           shadowSubMeshes;
        std::shared_ptr<const StaticMeshCollider> collider; // def.collision
        DirectX::XMFLOAT4X4                  prevWorld = {};
        bool                                 prevWorldValid = false;
        // point_light: position follows the part every frame
        LightEnvironment::PointLight         light;
        // particle_emitter
        std::unique_ptr<ParticleSystem>      particles;
        // Kinematic collider (box, sphere, mesh collision) or the dynamic body.
        BodyHandle                           body = k_invalidBody;
        DirectX::XMFLOAT3                    bodyPosition = {}; // last pose sent to MoveKinematic
        DirectX::XMFLOAT4                    bodyRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    };
    struct LoadedActor
    {
        SceneDescriptor::ActorEntry def;      // level placement state (+ resolved parts)
        Entity*                     entity    = nullptr;
        TransformComponent*         transform = nullptr;
        std::vector<ActorPart>      parts;
        bool                        spawned   = false; // fox.world.spawn: never saved
        int                         dynamicPart = -1;  // index of the part whose body moves the actor
    };
    // Main-thread lookup of an actor class (path -> components); the
    // ScriptSystem installs it while running. Without it actors have no parts.
    std::function<ResolvedActorClass(const std::string& classPath)> actorClassResolver;

    // Creates an actor at runtime (not saved). Meshes the level did not load
    // are imported synchronously. Returns null when the class does not
    // resolve (logged). def.resolved is filled from cls and def.components.
    LoadedActor* SpawnActor(SceneDescriptor::ActorEntry def, const ResolvedActorClass& cls);
    // Removes the actor, its parts, bodies and entities (and detaches any
    // other entity parented to it).
    void DestroyActor(EntityID root);
    LoadedActor* FindActor(EntityID root);
    const LoadedActor* FindActor(EntityID root) const;
    // The actor and part a part entity belongs to, or null.
    ActorPart* FindActorPart(EntityID partEntity, LoadedActor** actor = nullptr);

    // Editor tooling for placed actors (indices into actors). Each call
    // (re)builds the actor from its placement through actorClassResolver,
    // so it also picks up class edits; entity ids change on every rebuild.
    // Add/Duplicate return the new index, or -1 without a scene.
    int AddActor(const std::string& classPath, const DirectX::XMFLOAT3& position);
    // The placement with the root's current transform (what would be saved).
    SceneDescriptor::ActorEntry ActorPlacement(int index) const;
    SceneDescriptor::ActorEntry RemoveActor(int index);
    void InsertActor(int index, SceneDescriptor::ActorEntry def);
    int DuplicateActor(int index);
    void RebuildActor(int index);
    // Rebuilds the actor's parts from its class after a class edit (hot
    // reload), keeping the root entity, its transform and its scripts. Part
    // entities are replaced. On a class or override error the current parts
    // stay and false is returned (logged).
    bool ReloadActorParts(LoadedActor& actor);

    // Off: Update animates (particles, flicker) but does not step physics,
    // so dynamic actors keep their placement (editor edit mode).
    bool simulatePhysics = true;

    // Cascaded shadow range in metres; 0 keeps the camera far plane.
    float& ShadowDistance() { return m_shadowDistance; }
    float  ShadowDistance() const { return m_shadowDistance; }

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
    MotionBlurParams       motionBlur;
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

    std::vector<LoadedMesh>                        loadedMeshes;
    std::vector<LoadedObject>                      sceneObjects;
    std::vector<LoadedEmpty>                       emptyEntities;
    std::vector<LoadedActor>                       actors;
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
    std::string                                    lastRequestedSceneJson; // in-memory level text of that request (empty = file)
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
    // Declared after physicsWorld and world: vehicles and balls are destroyed first.
    std::vector<std::unique_ptr<VehicleInstance>> vehicles;
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
    void PumpVehicleSpawn();

    // Builds the actor's root components, parts (child entities of root in
    // scene) and bodies into the given world/physics: the incoming ones
    // during a scene apply. loadMissing imports meshes the asset cache lacks
    // (runtime spawns).
    bool BuildActor(LoadedActor& actor, Scene& scene, Entity& root, PhysicsWorld& physics,
                    bool loadMissing);
    void CreatePartBody(LoadedActor& actor, ActorPart& part, PhysicsWorld& physics);
    // The parts of BuildActor: from actor.def.resolved under actor.entity.
    void BuildActorParts(LoadedActor& actor, Scene& scene, PhysicsWorld& physics, bool loadMissing);
    // Resolves def through actorClassResolver (logging why it has no parts)
    // and builds it as a placed actor in the current world.
    LoadedActor CreatePlacedActor(SceneDescriptor::ActorEntry def);
    // Part visibility, light/emitter state and body presence from the root's
    // active flag and each part's enabled flag.
    void RefreshActorState(LoadedActor& actor);
    // Lights, emitters and kinematic bodies follow their parts; call before
    // the physics step.
    void UpdateActors();
    // Mesh, materials and (when collision) collider of a part mesh: from the
    // asset cache, or imported now when loadMissing.
    bool LoadActorMesh(ActorPart& part, bool loadMissing);
    // Box/sphere parts render a mesh built at their exact size, so editor
    // picking and framing read correct bounds. Shared by every part of the
    // same shape and size for the session.
    void LoadActorPrimitive(ActorPart& part);
    std::unordered_map<std::string, AssetHandle<Mesh>> m_primitiveMeshes;
    // Point lights of enabled actor parts, packed for LightEnvironment::BindPS.
    std::vector<LightEnvironment::PointLight> m_actorLights;

    // One drawable mesh (a level/actor mesh or vehicle part) with the data
    // every render phase needs, so the shadow, reflection and lit passes stay
    // in step instead of duplicating the iteration.
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
    template <typename Fn> void ForEachLoadedMeshInstance(Fn& fn) const;
    template <typename Fn> void ForEachActorMeshInstance(Fn& fn) const;
    template <typename Fn> void ForEachVehicleMeshInstance(Fn& fn) const;

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
    float             m_shadowDistance        = 0.0f; // metres; 0 = camera far plane
    uint32_t          m_cutoutFrameIndex      = 0;    // cutout dither animation step
    MotionBlur        m_motionBlur;
    float             m_lastUpdateDt          = 0.0f;

    LevelLoadOperation m_levelLoad;
    std::string        m_pendingSceneLoad;
    std::string        m_loadingScenePath;
    bool               m_completeSceneLoadNextFrame = false;

    // Body + wheel meshes import on MeshAddOperation workers while the vehicle
    // id is already handed to scripts.
    struct PendingVehicle
    {
        uint32_t          id = 0;
        VehicleDesc       desc;
        DirectX::XMFLOAT3 groundPosition = {};
        float             yawDeg = 0.0f;
        // Body, wheels, then spindles; operations of absent optional parts stay Idle.
        std::array<MeshAddOperation, 1 + 2 * VehicleDesc::WheelCount> meshes;
    };
    std::unique_ptr<PendingVehicle> m_pendingVehicle;
    std::string                     m_vehicleSpawnError;
    uint32_t                        m_nextVehicleId = 1;
};

} // namespace SE
