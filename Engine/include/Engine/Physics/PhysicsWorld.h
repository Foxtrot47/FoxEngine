#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "Engine/Physics/Ray.h"
#include "Engine/Physics/OBB.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Physics/RigidBodyComponent.h"
#include "Engine/Scene/TransformComponent.h"

namespace SE {

class PhysicsWorld;
struct StaticMeshCollider; // opaque; see PhysicsWorld::BuildStaticMeshCollider

// Opaque body identifier (Jolt BodyID value).
using BodyHandle = uint32_t;
inline constexpr BodyHandle k_invalidBody = 0xFFFFFFFFu;

// Receives a callback before every fixed simulation step, on the thread that
// calls PhysicsWorld::Step. Forces added here act during that step.
class PhysicsStepListener
{
public:
    virtual ~PhysicsStepListener() = default;
    virtual void OnPhysicsStep(PhysicsWorld& world, float dt) = 0;
};

// Rigid-body world backed by Jolt Physics. Jolt types stay inside the
// implementation so engine/game/editor code never sees Jolt headers.
//
// Simulation runs at a fixed rate (Settings::fixedTimeStep); Step(dt)
// consumes frame time in whole fixed steps. Dynamic bodies are mirrored to
// their TransformComponent / RigidBodyComponent after each step. Edits made
// to those components between steps (teleports, velocity, mass, friction,
// restitution, gravity) are detected and pushed back into the simulation, so
// tooling can keep editing plain component fields.
//
// The world is created lazily on first use; a default-constructed
// PhysicsWorld holds no Jolt state. Moving a world keeps body handles and
// listeners valid, but anything holding a PhysicsWorld* must be re-pointed.
class PhysicsWorld
{
public:
    struct Settings
    {
        float             fixedTimeStep = 1.0f / 120.0f;
        int               maxStepsPerFrame = 8; // excess frame time is dropped
        DirectX::XMFLOAT3 gravity = { 0.0f, -9.81f, 0.0f };
    };

    struct RaycastHit
    {
        enum class Kind { Sphere, Box, Convex, Mesh };

        float               t         = 0.0f;
        DirectX::XMFLOAT3   point     = {};
        DirectX::XMFLOAT3   normal    = {};
        DirectX::XMFLOAT3   surfaceVelocity = {}; // velocity of the hit body at point
        TransformComponent* transform = nullptr;  // set for mirrored dynamic bodies
        BodyHandle          body      = k_invalidBody;
        bool                dynamic   = false;
        Kind                kind      = Kind::Box;
    };

    // Convex dynamic body with an explicit mass distribution. Mass comes
    // from the RigidBodyComponent. Damping and sleeping default to Jolt's;
    // bodies driven by modelled external forces every step turn them off.
    struct ConvexBodyDesc
    {
        std::vector<DirectX::XMFLOAT3> hullPoints;   // body-local
        DirectX::XMFLOAT3 centerOfMass = {};          // body-local
        DirectX::XMFLOAT3 inertia = { 1.0f, 1.0f, 1.0f }; // principal moments about the
                                                      // center of mass along body X/Y/Z, kg*m^2
        float linearDamping  = 0.05f; // 1/s
        float angularDamping = 0.05f; // 1/s
        bool  allowSleeping  = true;
    };

    // Current simulation state (not render-interpolated).
    struct BodyState
    {
        DirectX::XMFLOAT3 position        = {}; // body origin
        DirectX::XMFLOAT4 rotation        = { 0.0f, 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT3 centerOfMass    = {}; // world
        DirectX::XMFLOAT3 linearVelocity  = {};
        DirectX::XMFLOAT3 angularVelocity = {};
    };

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    const Settings& GetSettings() const { return m_settings; }

    // Dynamic bodies are mirrored to t / rb, which must outlive the body
    // (they are owned by the scene entity). Return k_invalidBody on failure.
    BodyHandle AddSphere(TransformComponent* t, RigidBodyComponent* rb, float radius);
    BodyHandle AddConvexBody(TransformComponent* t, RigidBodyComponent* rb,
                             const ConvexBodyDesc& desc);
    BodyHandle AddStaticBox(const OBB& box, float friction = 0.4f, float restitution = 0.5f);
    // Static triangle-mesh collision (level geometry). Building is the
    // expensive part and is thread-safe, so loaders do it on worker threads;
    // the immutable collider can be shared by any number of bodies/worlds.
    // Triangles use the engine's winding (clockwise front faces in LH space).
    static std::shared_ptr<const StaticMeshCollider> BuildStaticMeshCollider(
        const std::vector<DirectX::XMFLOAT3>& vertices, const std::vector<uint32_t>& indices,
        std::string* error = nullptr);
    BodyHandle AddStaticMesh(const std::shared_ptr<const StaticMeshCollider>& collider,
                             DirectX::XMFLOAT3 position, DirectX::XMFLOAT3 eulerDeg,
                             DirectX::XMFLOAT3 scale, float friction = 0.8f);
    void RemoveBody(BodyHandle body);

    BodyState         GetBodyState(BodyHandle body) const;
    DirectX::XMFLOAT3 GetPointVelocity(BodyHandle body, DirectX::XMFLOAT3 worldPoint) const;
    // Accumulated until the end of the next fixed step; ignored for
    // non-dynamic bodies.
    void AddForceAtPoint(BodyHandle body, DirectX::XMFLOAT3 force, DirectX::XMFLOAT3 worldPoint);
    void SetAngularVelocity(BodyHandle body, DirectX::XMFLOAT3 angularVelocity);

    void AddStepListener(PhysicsStepListener* listener);
    void RemoveStepListener(PhysicsStepListener* listener);

    // Closest hit along the ray, ignoring the character and `ignore`.
    bool Raycast(const Ray& ray, RaycastHit& hit, float maxDistance = 10000.0f,
                 BodyHandle ignore = k_invalidBody) const;

    // Moves the character capsule (feet at cc.position) with gravity, stair
    // stepping and collision against all bodies; pushes dynamic bodies.
    // wishVel is the desired horizontal velocity in m/s.
    void StepCharacter(CharacterController& cc, DirectX::XMFLOAT3 wishVel, float dt);

    // Advances the simulation by dt seconds of frame time (0 pauses).
    void Step(float dt);

private:
    struct Impl;
    Impl& GetImpl();

    Settings              m_settings;
    std::unique_ptr<Impl> m_impl;
};

} // namespace SE
