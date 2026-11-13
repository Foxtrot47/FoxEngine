#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Core/Logger.h"

// Jolt must be included first: it configures platform macros for the rest.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace SE {

using namespace DirectX;

namespace {

// --- Process-wide Jolt registration (Factory + type registry) ---------------

void JoltTrace(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    SE_LOG_INFO("Jolt: %s", buffer);
}

std::mutex s_joltMutex;
int        s_joltUsers = 0;

// Jolt's factory and type registry are global; worlds share them and the
// last world to go away tears them down.
struct JoltLibrary
{
    JoltLibrary()
    {
        std::lock_guard<std::mutex> lock(s_joltMutex);
        if (s_joltUsers++ > 0)
            return;
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltLibrary()
    {
        std::lock_guard<std::mutex> lock(s_joltMutex);
        if (--s_joltUsers > 0)
            return;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    JoltLibrary(const JoltLibrary&) = delete;
    JoltLibrary& operator=(const JoltLibrary&) = delete;
};

// --- Collision layers ---------------------------------------------------------

// Kinematic bodies get their own layer so the pair filters below skip
// kinematic-vs-static and kinematic-vs-kinematic tests (Jolt would discard
// those pairs anyway, but only after the broad phase found them).
namespace Layers {
constexpr JPH::ObjectLayer Static    = 0;
constexpr JPH::ObjectLayer Dynamic   = 1;
constexpr JPH::ObjectLayer Kinematic = 2;
constexpr JPH::ObjectLayer Count     = 3;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer Static(0);
constexpr JPH::BroadPhaseLayer Dynamic(1);
constexpr JPH::uint            Count = 2;
}

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::Static ? BroadPhaseLayers::Static : BroadPhaseLayers::Dynamic;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseLayers::Static ? "Static" : "Dynamic";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
    {
        return layer == Layers::Dynamic || bpLayer == BroadPhaseLayers::Dynamic;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        return a == Layers::Dynamic || b == Layers::Dynamic;
    }
};

// --- Conversions -----------------------------------------------------------------

JPH::Vec3 ToJolt(const XMFLOAT3& v) { return JPH::Vec3(v.x, v.y, v.z); }
XMFLOAT3 ToSE(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }

JPH::Quat ToJolt(const XMFLOAT4& q)
{
    const JPH::Quat j(q.x, q.y, q.z, q.w);
    return j.LengthSq() > 1e-12f ? j.Normalized() : JPH::Quat::sIdentity();
}

bool operator==(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool operator!=(const XMFLOAT3& a, const XMFLOAT3& b) { return !(a == b); }

// TransformComponent stores XMMatrixRotationRollPitchYaw angles; the same
// quaternion rotates vectors identically in DirectXMath and Jolt.
JPH::Quat EulerDegToQuat(const XMFLOAT3& eulerDeg)
{
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(XMConvertToRadians(eulerDeg.x),
                                                       XMConvertToRadians(eulerDeg.y),
                                                       XMConvertToRadians(eulerDeg.z)));
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}

// Inverse of EulerDegToQuat (R = Rz(roll) * Rx(pitch) * Ry(yaw), row vectors).
XMFLOAT3 QuatToEulerDeg(JPH::QuatArg q)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(
        XMVectorSet(q.GetX(), q.GetY(), q.GetZ(), q.GetW())));
    const float sinPitch = std::clamp(-m.m[2][1], -1.0f, 1.0f);
    const float pitch    = asinf(sinPitch);
    float yaw, roll;
    if (fabsf(cosf(pitch)) > 1e-4f)
    {
        yaw  = atan2f(m.m[2][0], m.m[2][2]);
        roll = atan2f(m.m[0][1], m.m[1][1]);
    }
    else // gimbal lock: fold roll into yaw
    {
        yaw  = atan2f(-m.m[0][2], m.m[0][0]);
        roll = 0.0f;
    }
    return { XMConvertToDegrees(pitch), XMConvertToDegrees(yaw), XMConvertToDegrees(roll) };
}

JPH::Quat AxesToQuat(const OBB& box)
{
    const XMMATRIX basis(
        XMVectorSetW(XMLoadFloat3(&box.axes[0]), 0.0f),
        XMVectorSetW(XMLoadFloat3(&box.axes[1]), 0.0f),
        XMVectorSetW(XMLoadFloat3(&box.axes[2]), 0.0f),
        XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f));
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationMatrix(basis));
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}

constexpr float k_contactSlop = 0.05f; // character contacts closer than this count as touching

// Collects contacts that begin during PhysicsSystem::Update. Jolt calls it
// from job threads with all bodies locked; Step reports them afterwards.
class ContactCollector final : public JPH::ContactListener
{
public:
    explicit ContactCollector(const JPH::PhysicsSystem& system) : m_system(system) {}

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override
    {
        // Jolt reports each sub-shape pair (mesh triangle); another one
        // touching last step means these bodies were already in contact.
        if (m_system.WereBodiesInContact(body1.GetID(), body2.GetID()))
            return;
        const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
        JPH::Vec3 sum = JPH::Vec3::sZero();
        for (JPH::uint i = 0; i < count; ++i)
            sum += manifold.GetWorldSpaceContactPointOn1(i) + manifold.GetWorldSpaceContactPointOn2(i);
        const JPH::Vec3 point = sum / (2.0f * static_cast<float>((std::max)(count, 1u)));

        PhysicsContact contact;
        contact.bodyA = body1.GetID().GetIndexAndSequenceNumber();
        contact.bodyB = body2.GetID().GetIndexAndSequenceNumber();
        contact.point = ToSE(point);
        contact.normal = ToSE(manifold.mWorldSpaceNormal);
        contact.relativeVelocity = ToSE(body1.GetPointVelocity(point) - body2.GetPointVelocity(point));

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const PhysicsContact& existing : m_contacts) // another sub-shape pair in this step
            if ((existing.bodyA == contact.bodyA && existing.bodyB == contact.bodyB) ||
                (existing.bodyA == contact.bodyB && existing.bodyB == contact.bodyA))
                return;
        m_contacts.push_back(contact);
    }

    // In body order, so dispatch does not depend on job scheduling.
    std::vector<PhysicsContact> Take()
    {
        std::vector<PhysicsContact> contacts;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            contacts.swap(m_contacts);
        }
        std::sort(contacts.begin(), contacts.end(), [](const PhysicsContact& a, const PhysicsContact& b) {
            return a.bodyA != b.bodyA ? a.bodyA < b.bodyA : a.bodyB < b.bodyB;
        });
        return contacts;
    }

private:
    const JPH::PhysicsSystem&   m_system;
    std::mutex                  m_mutex;
    std::vector<PhysicsContact> m_contacts;
};

} // namespace

// Immutable, shareable level collision. Holds a Jolt registration so the
// shape can be built and destroyed on any thread, independent of worlds.
struct StaticMeshCollider
{
    JoltLibrary               library;
    JPH::RefConst<JPH::Shape> shape;
};

// --- Implementation --------------------------------------------------------------

struct PhysicsWorld::Impl
{
    // Last state written to (or read from) the scene components. A mismatch
    // on the next step means tooling edited the component.
    struct DynamicBody
    {
        JPH::BodyID          id;
        TransformComponent*  transform = nullptr;
        RigidBodyComponent*  body      = nullptr;

        XMFLOAT3  writtenPosition = {};
        XMFLOAT3  writtenEuler    = {};
        XMFLOAT3  writtenVelocity = {};
        float     appliedMass        = 0.0f;
        float     appliedFriction    = 0.0f;
        float     appliedRestitution = 0.0f;
        bool      appliedGravity     = true;
        bool      appliedEnabled     = true;

        // Simulation state before/after the last fixed step, for rendering
        // interpolation between steps.
        JPH::Vec3 previousPosition, currentPosition;
        JPH::Quat previousRotation, currentRotation;
    };

    struct Character
    {
        JPH::Ref<JPH::CharacterVirtual> virtualCharacter;
        float    radius = 0.0f;
        float    height = 0.0f;
        float    slopeLimit = 0.0f;
        XMFLOAT3 writtenPosition = {};
    };

    struct KinematicBody
    {
        JPH::BodyID id;
        bool        hasTarget = false; // MoveKinematic target waiting for the next steps
        JPH::RVec3  targetPosition;
        JPH::Quat   targetRotation;
        bool        moving = false;    // driven toward a target during the last Step
    };

    explicit Impl(const Settings& settings)
        : tempAllocator(16 * 1024 * 1024)
        , jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
               static_cast<int>((std::max)(1u, std::thread::hardware_concurrency()) - 1))
    {
        system.Init(65536, 0, 65536, 16384, broadPhaseLayers, objectVsBroadPhase, objectPairs);
        system.SetGravity(ToJolt(settings.gravity));
        system.SetContactListener(&contacts);
    }

    ~Impl()
    {
        character.virtualCharacter = nullptr;
        RemoveAllBodies();
    }

    JPH::BodyInterface& Bodies() { return system.GetBodyInterface(); }

    void RemoveAllBodies()
    {
        JPH::BodyIDVector ids;
        for (const auto& d : dynamics) ids.push_back(d.id);
        for (const auto& k : kinematics) ids.push_back(k.id);
        ids.insert(ids.end(), statics.begin(), statics.end());
        if (!ids.empty())
        {
            Bodies().RemoveBodies(ids.data(), static_cast<int>(ids.size()));
            Bodies().DestroyBodies(ids.data(), static_cast<int>(ids.size()));
        }
        dynamics.clear();
        kinematics.clear();
        statics.clear();
    }

    KinematicBody* FindKinematic(JPH::BodyID id)
    {
        auto it = std::find_if(kinematics.begin(), kinematics.end(),
                               [id](const KinematicBody& k) { return k.id == id; });
        return it != kinematics.end() ? &*it : nullptr;
    }

    void CaptureCurrent(DynamicBody& d)
    {
        Bodies().GetPositionAndRotation(d.id, d.currentPosition, d.currentRotation);
    }

    // Push component edits made since the last step into the simulation.
    void PushEdits()
    {
        auto& bi = Bodies();
        for (auto& d : dynamics)
        {
            TransformComponent& t = *d.transform;
            RigidBodyComponent& rb = *d.body;
            bool wake = false;
            if (t.position != d.writtenPosition || t.eulerDeg != d.writtenEuler)
            {
                bi.SetPositionAndRotation(d.id, ToJolt(t.position), EulerDegToQuat(t.eulerDeg),
                                          JPH::EActivation::DontActivate);
                CaptureCurrent(d);
                d.previousPosition = d.currentPosition;
                d.previousRotation = d.currentRotation;
                d.writtenPosition = t.position;
                d.writtenEuler = t.eulerDeg;
                wake = true;
            }
            if (rb.velocity != d.writtenVelocity)
            {
                bi.SetLinearVelocity(d.id, ToJolt(rb.velocity));
                d.writtenVelocity = rb.velocity;
                wake = true;
            }
            if (rb.friction != d.appliedFriction)
            {
                bi.SetFriction(d.id, rb.friction);
                d.appliedFriction = rb.friction;
            }
            if (rb.restitution != d.appliedRestitution)
            {
                bi.SetRestitution(d.id, rb.restitution);
                d.appliedRestitution = rb.restitution;
            }
            if (rb.useGravity != d.appliedGravity)
            {
                bi.SetGravityFactor(d.id, rb.useGravity ? 1.0f : 0.0f);
                d.appliedGravity = rb.useGravity;
                wake = true;
            }
            if (rb.mass != d.appliedMass && rb.mass > 0.0f)
            {
                JPH::BodyLockWrite lock(system.GetBodyLockInterface(), d.id);
                if (lock.Succeeded())
                    lock.GetBody().GetMotionProperties()->ScaleToMass(rb.mass);
                d.appliedMass = rb.mass;
            }
            if (rb.enabled != d.appliedEnabled)
            {
                d.appliedEnabled = rb.enabled;
                if (!rb.enabled)
                    bi.DeactivateBody(d.id);
                wake = rb.enabled;
            }
            if (wake && rb.enabled)
                bi.ActivateBody(d.id);
        }
    }

    // Write interpolated simulation state back to the components.
    void PullState(float alpha)
    {
        auto& bi = Bodies();
        for (auto& d : dynamics)
        {
            const JPH::Vec3 p = d.previousPosition + (d.currentPosition - d.previousPosition) * alpha;
            const JPH::Quat q = d.previousRotation.SLERP(d.currentRotation, alpha).Normalized();
            d.writtenPosition = ToSE(p);
            d.writtenEuler    = QuatToEulerDeg(q);
            d.writtenVelocity = ToSE(bi.GetLinearVelocity(d.id));
            d.transform->position = d.writtenPosition;
            d.transform->eulerDeg = d.writtenEuler;
            d.body->velocity      = d.writtenVelocity;
        }
    }

    BodyHandle AddMirroredDynamic(JPH::BodyCreationSettings& bcs, TransformComponent* t,
                                  RigidBodyComponent* rb)
    {
        bcs.mFriction       = rb->friction;
        bcs.mRestitution    = rb->restitution;
        bcs.mGravityFactor  = rb->useGravity ? 1.0f : 0.0f;
        bcs.mLinearVelocity = ToJolt(rb->velocity);
        bcs.mUserData       = reinterpret_cast<JPH::uint64>(t);

        DynamicBody d;
        d.id = Bodies().CreateAndAddBody(bcs, rb->enabled ? JPH::EActivation::Activate
                                                          : JPH::EActivation::DontActivate);
        if (d.id.IsInvalid())
        {
            SE_LOG_ERROR("Physics: body limit reached, dynamic body not added");
            return k_invalidBody;
        }
        d.transform = t;
        d.body      = rb;
        d.writtenPosition    = t->position;
        d.writtenEuler       = t->eulerDeg;
        d.writtenVelocity    = rb->velocity;
        d.appliedMass        = rb->mass;
        d.appliedFriction    = rb->friction;
        d.appliedRestitution = rb->restitution;
        d.appliedGravity     = rb->useGravity;
        d.appliedEnabled     = rb->enabled;
        CaptureCurrent(d);
        d.previousPosition = d.currentPosition;
        d.previousRotation = d.currentRotation;
        dynamics.push_back(d);
        return d.id.GetIndexAndSequenceNumber();
    }

    TransformComponent* Owner(BodyHandle body) const
    {
        JPH::BodyLockRead lock(system.GetBodyLockInterface(), JPH::BodyID(body));
        return lock.Succeeded() ? reinterpret_cast<TransformComponent*>(lock.GetBody().GetUserData()) : nullptr;
    }

    // Member order matters: the library guard must outlive everything below.
    JoltLibrary                library;
    JPH::TempAllocatorImpl     tempAllocator;
    JPH::JobSystemThreadPool   jobs;
    BroadPhaseLayerMap         broadPhaseLayers;
    ObjectVsBroadPhaseFilter   objectVsBroadPhase;
    ObjectPairFilter           objectPairs;
    JPH::PhysicsSystem         system;
    ContactCollector           contacts{ system };

    std::vector<DynamicBody>          dynamics;
    std::vector<KinematicBody>        kinematics;
    std::vector<JPH::BodyID>          statics;
    std::vector<PhysicsStepListener*> listeners;
    Character                         character;
    float                             accumulator = 0.0f;
    bool                              broadPhaseDirty = false;
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

PhysicsWorld::Impl& PhysicsWorld::GetImpl()
{
    if (!m_impl)
        m_impl = std::make_unique<Impl>(m_settings);
    return *m_impl;
}

BodyHandle PhysicsWorld::AddSphere(TransformComponent* t, RigidBodyComponent* rb, float radius)
{
    Impl& im = GetImpl(); // registers Jolt's allocator before any shape is created
    JPH::BodyCreationSettings bcs(new JPH::SphereShape((std::max)(radius, 0.01f)),
                                  ToJolt(t->position), EulerDegToQuat(t->eulerDeg),
                                  JPH::EMotionType::Dynamic, Layers::Dynamic);
    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast; // no tunneling when fast
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bcs.mMassPropertiesOverride.mMass = (std::max)(rb->mass, 0.001f);
    return im.AddMirroredDynamic(bcs, t, rb);
}

BodyHandle PhysicsWorld::AddConvexBody(TransformComponent* t, RigidBodyComponent* rb,
                                       const ConvexBodyDesc& desc)
{
    Impl& im = GetImpl();
    JPH::Array<JPH::Vec3> points;
    points.reserve(desc.hullPoints.size());
    for (const XMFLOAT3& p : desc.hullPoints)
        points.push_back(ToJolt(p));
    JPH::ConvexHullShapeSettings hullSettings(points);
    JPH::ShapeSettings::ShapeResult hull = hullSettings.Create();
    if (hull.HasError())
    {
        SE_LOG_ERROR("Physics: convex hull failed: %s", hull.GetError().c_str());
        return k_invalidBody;
    }
    // Move the hull's volumetric center of mass to the authored one.
    const JPH::Vec3 offset = ToJolt(desc.centerOfMass) - hull.Get()->GetCenterOfMass();
    JPH::OffsetCenterOfMassShapeSettings offsetSettings(offset, hull.Get());
    JPH::ShapeSettings::ShapeResult shape = offsetSettings.Create();
    if (shape.HasError())
    {
        SE_LOG_ERROR("Physics: center of mass offset failed: %s", shape.GetError().c_str());
        return k_invalidBody;
    }

    JPH::BodyCreationSettings bcs(shape.Get(), ToJolt(t->position), EulerDegToQuat(t->eulerDeg),
                                  JPH::EMotionType::Dynamic, Layers::Dynamic);
    bcs.mMotionQuality  = JPH::EMotionQuality::LinearCast;
    bcs.mAllowSleeping  = desc.allowSleeping;
    bcs.mLinearDamping  = desc.linearDamping;
    bcs.mAngularDamping = desc.angularDamping;
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    bcs.mMassPropertiesOverride.mMass = (std::max)(rb->mass, 0.001f);
    bcs.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(ToJolt(desc.inertia));
    return im.AddMirroredDynamic(bcs, t, rb);
}

std::shared_ptr<const StaticMeshCollider> PhysicsWorld::BuildStaticMeshCollider(
    const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices, std::string* error)
{
    auto collider = std::make_shared<StaticMeshCollider>(); // registers Jolt first
    JPH::VertexList joltVertices;
    joltVertices.reserve(vertices.size());
    for (const XMFLOAT3& v : vertices)
        joltVertices.push_back(JPH::Float3(v.x, v.y, v.z));
    JPH::IndexedTriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));
    if (triangles.empty())
    {
        if (error) *error = "mesh has no triangles";
        return nullptr;
    }
    // Removes degenerate and duplicate triangles before building the tree.
    JPH::MeshShapeSettings settings(std::move(joltVertices), std::move(triangles));
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError())
    {
        if (error) *error = result.GetError().c_str();
        return nullptr;
    }
    collider->shape = result.Get();
    return collider;
}

BodyHandle PhysicsWorld::AddStaticMesh(const std::shared_ptr<const StaticMeshCollider>& collider,
                                       XMFLOAT3 position, XMFLOAT3 eulerDeg, XMFLOAT3 scale,
                                       float friction)
{
    if (!collider)
        return k_invalidBody;
    Impl& im = GetImpl();
    JPH::RefConst<JPH::Shape> shape = collider->shape;
    if (scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f)
        shape = new JPH::ScaledShape(shape, ToJolt(scale));
    JPH::BodyCreationSettings bcs(shape, ToJolt(position), EulerDegToQuat(eulerDeg),
                                  JPH::EMotionType::Static, Layers::Static);
    bcs.mFriction    = friction;
    bcs.mRestitution = 0.0f;
    const JPH::BodyID id = im.Bodies().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
    {
        SE_LOG_ERROR("Physics: body limit reached, mesh collider not added");
        return k_invalidBody;
    }
    im.statics.push_back(id);
    im.broadPhaseDirty = true;
    return id.GetIndexAndSequenceNumber();
}

BodyHandle PhysicsWorld::AddStaticBox(const OBB& box, float friction, float restitution)
{
    Impl& im = GetImpl();
    const JPH::Vec3 halfExtents(
        (std::max)(box.halfExtents.x, 0.01f), (std::max)(box.halfExtents.y, 0.01f),
        (std::max)(box.halfExtents.z, 0.01f));
    // Convex radius must not exceed the smallest half extent.
    const float convexRadius = (std::min)(JPH::cDefaultConvexRadius, halfExtents.ReduceMin());
    JPH::BodyCreationSettings bcs(new JPH::BoxShape(halfExtents, convexRadius),
                                  ToJolt(box.center), AxesToQuat(box),
                                  JPH::EMotionType::Static, Layers::Static);
    bcs.mFriction    = friction;
    bcs.mRestitution = restitution;
    const JPH::BodyID id = im.Bodies().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
    {
        SE_LOG_ERROR("Physics: body limit reached, box not added");
        return k_invalidBody;
    }
    im.statics.push_back(id);
    im.broadPhaseDirty = true;
    return id.GetIndexAndSequenceNumber();
}

BodyHandle PhysicsWorld::AddKinematicBody(const KinematicShape& shape, XMFLOAT3 position,
                                          XMFLOAT4 rotation, float friction, float restitution,
                                          TransformComponent* owner)
{
    Impl& im = GetImpl();
    JPH::RefConst<JPH::Shape> joltShape;
    switch (shape.type)
    {
    case KinematicShape::Type::Box:
    {
        const JPH::Vec3 halfExtents(
            (std::max)(shape.halfExtents.x, 0.01f), (std::max)(shape.halfExtents.y, 0.01f),
            (std::max)(shape.halfExtents.z, 0.01f));
        const float convexRadius = (std::min)(JPH::cDefaultConvexRadius, halfExtents.ReduceMin());
        joltShape = new JPH::BoxShape(halfExtents, convexRadius);
        break;
    }
    case KinematicShape::Type::Sphere:
        joltShape = new JPH::SphereShape((std::max)(shape.radius, 0.01f));
        break;
    case KinematicShape::Type::Mesh:
        if (!shape.mesh)
        {
            SE_LOG_ERROR("Physics: kinematic mesh body has no collider, not added");
            return k_invalidBody;
        }
        joltShape = shape.mesh->shape;
        if (shape.scale.x != 1.0f || shape.scale.y != 1.0f || shape.scale.z != 1.0f)
            joltShape = new JPH::ScaledShape(joltShape, ToJolt(shape.scale));
        break;
    }

    JPH::BodyCreationSettings bcs(joltShape, ToJolt(position), ToJolt(rotation),
                                  JPH::EMotionType::Kinematic, Layers::Kinematic);
    bcs.mFriction    = friction;
    bcs.mRestitution = restitution;
    bcs.mUserData    = reinterpret_cast<JPH::uint64>(owner);
    // Mesh shapes have no computable mass; kinematic bodies ignore it anyway.
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    bcs.mMassPropertiesOverride.mMass    = 1.0f;
    bcs.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
    const JPH::BodyID id = im.Bodies().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
    {
        SE_LOG_ERROR("Physics: body limit reached, kinematic body not added");
        return k_invalidBody;
    }
    Impl::KinematicBody k;
    k.id = id;
    im.kinematics.push_back(k);
    im.broadPhaseDirty = true;
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::MoveKinematic(BodyHandle body, XMFLOAT3 position, XMFLOAT4 rotation,
                                 bool teleport)
{
    if (!m_impl || body == k_invalidBody)
        return;
    Impl::KinematicBody* k = m_impl->FindKinematic(JPH::BodyID(body));
    if (!k)
        return;
    if (teleport)
    {
        JPH::BodyInterface& bi = m_impl->Bodies();
        bi.SetPositionAndRotation(k->id, ToJolt(position), ToJolt(rotation),
                                  JPH::EActivation::Activate);
        bi.SetLinearAndAngularVelocity(k->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        k->hasTarget = false;
        k->moving    = false;
        return;
    }
    k->hasTarget      = true;
    k->targetPosition = ToJolt(position);
    k->targetRotation = ToJolt(rotation);
}

void PhysicsWorld::RemoveBody(BodyHandle body)
{
    if (!m_impl || body == k_invalidBody)
        return;
    Impl& im = *m_impl;
    const JPH::BodyID id(body);
    auto dyn = std::find_if(im.dynamics.begin(), im.dynamics.end(),
                            [id](const Impl::DynamicBody& d) { return d.id == id; });
    auto kin = std::find_if(im.kinematics.begin(), im.kinematics.end(),
                            [id](const Impl::KinematicBody& k) { return k.id == id; });
    auto stat = std::find(im.statics.begin(), im.statics.end(), id);
    if (dyn == im.dynamics.end() && kin == im.kinematics.end() && stat == im.statics.end())
        return;
    if (dyn != im.dynamics.end()) im.dynamics.erase(dyn);
    if (kin != im.kinematics.end()) im.kinematics.erase(kin);
    if (stat != im.statics.end()) im.statics.erase(stat);
    im.Bodies().RemoveBody(id);
    im.Bodies().DestroyBody(id);
}

PhysicsWorld::BodyState PhysicsWorld::GetBodyState(BodyHandle body) const
{
    BodyState state;
    if (!m_impl || body == k_invalidBody)
        return state;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), JPH::BodyID(body));
    if (!lock.Succeeded())
        return state;
    const JPH::Body& b = lock.GetBody();
    const JPH::Quat q = b.GetRotation();
    state.position        = ToSE(b.GetPosition());
    state.rotation        = { q.GetX(), q.GetY(), q.GetZ(), q.GetW() };
    state.centerOfMass    = ToSE(b.GetCenterOfMassPosition());
    state.linearVelocity  = ToSE(b.GetLinearVelocity());
    state.angularVelocity = ToSE(b.GetAngularVelocity());
    return state;
}

XMFLOAT3 PhysicsWorld::GetPointVelocity(BodyHandle body, XMFLOAT3 worldPoint) const
{
    if (!m_impl || body == k_invalidBody)
        return {};
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), JPH::BodyID(body));
    return lock.Succeeded() ? ToSE(lock.GetBody().GetPointVelocity(ToJolt(worldPoint)))
                            : XMFLOAT3{};
}

void PhysicsWorld::AddForceAtPoint(BodyHandle body, XMFLOAT3 force, XMFLOAT3 worldPoint)
{
    if (!m_impl || body == k_invalidBody)
        return;
    JPH::BodyLockWrite lock(m_impl->system.GetBodyLockInterface(), JPH::BodyID(body));
    if (lock.Succeeded() && lock.GetBody().IsDynamic())
        lock.GetBody().AddForce(ToJolt(force), ToJolt(worldPoint));
}

void PhysicsWorld::SetAngularVelocity(BodyHandle body, XMFLOAT3 angularVelocity)
{
    if (!m_impl || body == k_invalidBody)
        return;
    m_impl->Bodies().SetAngularVelocity(JPH::BodyID(body), ToJolt(angularVelocity));
}

void PhysicsWorld::AddStepListener(PhysicsStepListener* listener)
{
    auto& listeners = GetImpl().listeners;
    if (std::find(listeners.begin(), listeners.end(), listener) == listeners.end())
        listeners.push_back(listener);
}

void PhysicsWorld::RemoveStepListener(PhysicsStepListener* listener)
{
    if (!m_impl)
        return;
    auto& listeners = m_impl->listeners;
    listeners.erase(std::remove(listeners.begin(), listeners.end(), listener), listeners.end());
}

bool PhysicsWorld::Raycast(const Ray& ray, RaycastHit& hit, float maxDistance,
                           BodyHandle ignore) const
{
    if (!m_impl)
        return false;
    const JPH::RRayCast cast(ToJolt(ray.origin), ToJolt(ray.direction) * maxDistance);
    JPH::RayCastResult result;
    const JPH::IgnoreSingleBodyFilter ignoreFilter{ JPH::BodyID(ignore) };
    const JPH::BodyFilter anyBody;
    const JPH::BodyFilter& bodyFilter = ignore == k_invalidBody
        ? anyBody : static_cast<const JPH::BodyFilter&>(ignoreFilter);
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(cast, result, {}, {}, bodyFilter))
        return false;

    const JPH::Vec3 point = cast.GetPointOnRay(result.mFraction);
    hit.t     = result.mFraction * maxDistance;
    hit.point = ToSE(point);
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded())
        return false;
    const JPH::Body& body = lock.GetBody();
    hit.normal          = ToSE(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    hit.surfaceVelocity = ToSE(body.GetPointVelocity(point));
    hit.transform       = reinterpret_cast<TransformComponent*>(body.GetUserData());
    hit.body            = result.mBodyID.GetIndexAndSequenceNumber();
    hit.dynamic         = body.IsDynamic();
    switch (body.GetShape()->GetSubType())
    {
    case JPH::EShapeSubType::Sphere: hit.kind = RaycastHit::Kind::Sphere; break;
    case JPH::EShapeSubType::Box:    hit.kind = RaycastHit::Kind::Box; break;
    case JPH::EShapeSubType::Mesh:   hit.kind = RaycastHit::Kind::Mesh; break;
    case JPH::EShapeSubType::Scaled:
        hit.kind = static_cast<const JPH::ScaledShape*>(body.GetShape())->GetInnerShape()->GetSubType() ==
                   JPH::EShapeSubType::Mesh ? RaycastHit::Kind::Mesh : RaycastHit::Kind::Convex;
        break;
    default:                         hit.kind = RaycastHit::Kind::Convex; break;
    }
    return true;
}

void PhysicsWorld::StepCharacter(CharacterController& cc, XMFLOAT3 wishVel, float dt)
{
    if (dt <= 0.0f)
        return;
    Impl& im = GetImpl();
    Impl::Character& ch = im.character;

    if (!ch.virtualCharacter || ch.radius != cc.radius || ch.height != cc.height ||
        ch.slopeLimit != cc.slopeLimit)
    {
        const float halfHeight = (std::max)(0.5f * cc.height - cc.radius, 0.01f);
        JPH::CharacterVirtualSettings settings;
        settings.mShape = new JPH::CapsuleShape(halfHeight, cc.radius);
        // Feet sit at the character origin.
        settings.mShapeOffset = JPH::Vec3(0.0f, halfHeight + cc.radius, 0.0f);
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(cc.slopeLimit);
        settings.mUp = JPH::Vec3::sAxisY();
        // Contacts below the bottom hemisphere's center can support the character.
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -cc.radius);
        ch.virtualCharacter = new JPH::CharacterVirtual(&settings, ToJolt(cc.position),
                                                        JPH::Quat::sIdentity(), 0, &im.system);
        ch.radius = cc.radius;
        ch.height = cc.height;
        ch.slopeLimit = cc.slopeLimit;
        ch.writtenPosition = cc.position;
    }
    JPH::CharacterVirtual& character = *ch.virtualCharacter;
    if (cc.position != ch.writtenPosition) // teleported by game code
        character.SetPosition(ToJolt(cc.position));

    const bool onGround = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    if (!onGround && cc.gravityEnabled)
        cc.velY -= cc.gravAccel * dt;
    const float decay = fmaxf(0.0f, 1.0f - 6.0f * dt);
    cc.physVelX *= decay;
    cc.physVelZ *= decay;
    character.SetLinearVelocity(JPH::Vec3(wishVel.x + cc.physVelX, cc.velY,
                                          wishVel.z + cc.physVelZ));

    JPH::CharacterVirtual::ExtendedUpdateSettings update;
    update.mWalkStairsStepUp     = JPH::Vec3(0.0f, cc.stepHeight, 0.0f);
    update.mStickToFloorStepDown = JPH::Vec3(0.0f, -cc.stepHeight, 0.0f);
    const JPH::Vec3 gravity(0.0f, cc.gravityEnabled ? -cc.gravAccel : 0.0f, 0.0f);
    character.ExtendedUpdate(dt, gravity, update,
                             im.system.GetDefaultBroadPhaseLayerFilter(Layers::Dynamic),
                             im.system.GetDefaultLayerFilter(Layers::Dynamic),
                             {}, {}, im.tempAllocator);

    cc.position = ToSE(character.GetPosition());
    ch.writtenPosition = cc.position;

    // Any touching surface counts as grounded so Jump() can push off walls,
    // matching the previous controller; the normal sum picks the direction.
    XMFLOAT3 normalSum = { 0.0f, 0.0f, 0.0f };
    bool touching = false;
    bool ceiling  = false;
    for (const auto& contact : character.GetActiveContacts())
    {
        if (contact.mWasDiscarded || contact.mIsSensorB || contact.mDistance > k_contactSlop)
            continue;
        touching = true;
        normalSum.x += contact.mSurfaceNormal.GetX();
        normalSum.y += contact.mSurfaceNormal.GetY();
        normalSum.z += contact.mSurfaceNormal.GetZ();
        ceiling |= contact.mSurfaceNormal.GetY() < -0.5f;
    }
    const bool grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    cc.isGrounded = grounded || touching;
    if ((grounded && cc.velY < 0.0f) || (ceiling && cc.velY > 0.0f))
        cc.velY = 0.0f;
    const float len = sqrtf(normalSum.x * normalSum.x + normalSum.y * normalSum.y +
                            normalSum.z * normalSum.z);
    if (len > 1e-6f)
        cc.contactNormal = { normalSum.x / len, normalSum.y / len, normalSum.z / len };
}

void PhysicsWorld::Step(float dt)
{
    if (!m_impl)
        return;
    Impl& im = *m_impl;
    im.PushEdits();
    if (im.broadPhaseDirty)
    {
        im.system.OptimizeBroadPhase();
        im.broadPhaseDirty = false;
    }

    const float fixed = m_settings.fixedTimeStep;
    im.accumulator += (std::max)(dt, 0.0f);
    int steps = static_cast<int>(im.accumulator / fixed);
    if (steps > m_settings.maxStepsPerFrame)
    {
        steps = m_settings.maxStepsPerFrame;
        im.accumulator = 0.0f; // too far behind: drop time instead of spiralling
    }
    else
    {
        im.accumulator -= steps * fixed;
    }

    if (steps > 0)
    {
        JPH::BodyInterface& bi = im.Bodies();
        const float duration = steps * fixed;
        for (auto& k : im.kinematics)
        {
            if (!k.hasTarget)
                continue;
            bi.MoveKinematic(k.id, k.targetPosition, k.targetRotation, duration);
            k.hasTarget = false;
            k.moving    = true;
        }
    }

    for (int i = 0; i < steps; ++i)
    {
        for (auto& d : im.dynamics)
        {
            d.previousPosition = d.currentPosition;
            d.previousRotation = d.currentRotation;
        }
        for (size_t l = 0; l < im.listeners.size(); ++l)
            im.listeners[l]->OnPhysicsStep(*this, fixed);
        const JPH::EPhysicsUpdateError error =
            im.system.Update(fixed, 1, &im.tempAllocator, &im.jobs);
        if (error != JPH::EPhysicsUpdateError::None)
            SE_LOG_WARN("Physics: update error 0x%x", static_cast<unsigned>(error));
        for (auto& d : im.dynamics)
            im.CaptureCurrent(d);
    }
    // A kinematic body keeps its velocity after reaching the target; stop it
    // there unless a step listener already queued the next target.
    for (auto& k : im.kinematics)
    {
        if (!k.moving || k.hasTarget)
            continue;
        im.Bodies().SetLinearAndAngularVelocity(k.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        k.moving = false;
    }
    im.PullState(std::clamp(im.accumulator / fixed, 0.0f, 1.0f));

    // Owners are resolved per call: an earlier callback may have removed a body.
    for (PhysicsContact& contact : im.contacts.Take())
    {
        for (size_t l = 0; l < im.listeners.size(); ++l)
        {
            contact.ownerA = im.Owner(contact.bodyA);
            contact.ownerB = im.Owner(contact.bodyB);
            im.listeners[l]->OnPhysicsContact(*this, contact);
        }
    }
}

} // namespace SE
