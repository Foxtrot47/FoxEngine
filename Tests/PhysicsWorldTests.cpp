#include "Engine/Physics/PhysicsWorld.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace DirectX;

namespace {
void Require(bool value, const char* message)
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) < tolerance; }

// 60x60 slab whose top face is at y = 0.
SE::OBB Floor() { return SE::OBB::FromAABB({ -30.0f, -0.5f, -30.0f }, { 30.0f, 0.0f, 30.0f }); }

void Simulate(SE::PhysicsWorld& world, float seconds, float frameDt = 1.0f / 60.0f)
{
    for (float t = 0.0f; t < seconds; t += frameDt)
        world.Step(frameDt);
}

void TestBallSettlesOnFloor()
{
    SE::PhysicsWorld world;
    SE::TransformComponent t;
    SE::RigidBodyComponent rb;
    rb.restitution = 0.0f;
    t.position = { 0.0f, 5.0f, 0.0f };
    world.AddSphere(&t, &rb, 1.0f);
    world.AddStaticBox(Floor());

    Simulate(world, 0.5f);
    Require(t.position.y < 5.0f && rb.velocity.y < 0.0f, "ball falls and mirrors velocity");
    Simulate(world, 3.0f);
    Require(Near(t.position.y, 1.0f, 0.02f), "ball rests with its radius on the floor top");
    Require(Near(rb.velocity.y, 0.0f, 0.05f), "resting ball has no vertical velocity");
}

// Component edits between steps (tooling teleports, gravity toggle,
// velocity) must reach the simulation.
void TestComponentEditsArePushed()
{
    SE::PhysicsWorld world;
    SE::TransformComponent t;
    SE::RigidBodyComponent rb;
    t.position = { 0.0f, 10.0f, 0.0f };
    world.AddSphere(&t, &rb, 1.0f);
    world.AddStaticBox(Floor());
    Simulate(world, 0.5f);

    t.position = { 5.0f, 20.0f, -3.0f };
    rb.velocity = { 0.0f, 0.0f, 0.0f };
    rb.useGravity = false;
    Simulate(world, 1.0f);
    Require(Near(t.position.x, 5.0f, 1e-3f) && Near(t.position.y, 20.0f, 1e-3f) &&
            Near(t.position.z, -3.0f, 1e-3f), "teleport sticks and gravity toggle holds it");

    rb.velocity = { 2.0f, 0.0f, 0.0f };
    Simulate(world, 1.0f);
    Require(Near(t.position.x, 7.0f, 0.1f), "velocity edit drives the body");

    t.eulerDeg = { 10.0f, 20.0f, 30.0f };
    rb.velocity = { 0.0f, 0.0f, 0.0f };
    world.Step(0.0f);
    Require(Near(t.eulerDeg.x, 10.0f, 0.01f) && Near(t.eulerDeg.y, 20.0f, 0.01f) &&
            Near(t.eulerDeg.z, 30.0f, 0.01f), "euler angles round-trip through the simulation");
}

// A box tilted about Z (top normal leaning toward -X) must send a ball
// downhill toward -X; a transposed axis conversion would roll it to +X.
void TestTiltedBoxOrientation()
{
    SE::PhysicsWorld world;
    const float angle = XMConvertToRadians(25.0f);
    SE::OBB ramp;
    ramp.center      = { 0.0f, 0.0f, 0.0f };
    ramp.halfExtents = { 20.0f, 0.5f, 20.0f };
    ramp.axes[0] = { cosf(angle), sinf(angle), 0.0f };
    ramp.axes[1] = { -sinf(angle), cosf(angle), 0.0f };
    ramp.axes[2] = { 0.0f, 0.0f, 1.0f };
    world.AddStaticBox(ramp);

    SE::TransformComponent t;
    SE::RigidBodyComponent rb;
    t.position = { 0.0f, 3.0f, 0.0f };
    world.AddSphere(&t, &rb, 0.5f);
    Simulate(world, 1.5f);
    Require(t.position.x < -1.0f, "ball rolls down the tilted box toward -X");
    Require(std::fabs(t.position.z) < 0.05f, "no sideways drift on a pure Z tilt");
}

void TestRaycastReportsBodies()
{
    SE::PhysicsWorld world;
    SE::TransformComponent t;
    SE::RigidBodyComponent rb;
    t.position = { 10.0f, 1.0f, 0.0f };
    rb.useGravity = false;
    world.AddSphere(&t, &rb, 1.0f);
    world.AddStaticBox(Floor());
    world.Step(0.0f);

    SE::PhysicsWorld::RaycastHit hit;
    SE::Ray down;
    down.origin = { 0.0f, 10.0f, 0.0f };
    down.direction = { 0.0f, -1.0f, 0.0f };
    Require(world.Raycast(down, hit), "ray hits the floor");
    Require(hit.kind == SE::PhysicsWorld::RaycastHit::Kind::Box && !hit.transform, "floor is a static box");
    Require(Near(hit.t, 10.0f, 1e-3f) && Near(hit.point.y, 0.0f, 1e-3f), "floor hit distance");
    Require(Near(hit.normal.y, 1.0f, 1e-3f), "floor hit normal points up");

    SE::Ray sideways;
    sideways.origin = { 0.0f, 1.0f, 0.0f };
    sideways.direction = { 1.0f, 0.0f, 0.0f };
    Require(world.Raycast(sideways, hit), "ray hits the sphere");
    Require(hit.kind == SE::PhysicsWorld::RaycastHit::Kind::Sphere && hit.transform == &t,
            "sphere hit reports its transform");
    Require(Near(hit.t, 9.0f, 1e-3f) && Near(hit.normal.x, -1.0f, 1e-3f), "sphere hit geometry");
}

void TestCharacterLandsAndWalks()
{
    SE::PhysicsWorld world;
    world.AddStaticBox(Floor());
    SE::CharacterController cc;
    cc.position = { 0.0f, 3.0f, 0.0f };
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i)
        world.StepCharacter(cc, { 0.0f, 0.0f, 0.0f }, dt);
    Require(Near(cc.position.y, 0.0f, 0.05f), "character feet land on the floor");
    Require(cc.isGrounded && Near(cc.velY, 0.0f, 1e-3f), "landed character is grounded");
    Require(cc.contactNormal.y > 0.9f, "ground contact normal points up");

    for (int i = 0; i < 60; ++i)
        world.StepCharacter(cc, { 4.0f, 0.0f, 0.0f }, dt);
    Require(Near(cc.position.x, 4.0f, 0.2f), "character walks at the wished speed");
    Require(Near(cc.position.y, 0.0f, 0.05f), "walking keeps the feet on the floor");

    cc.Jump();
    world.StepCharacter(cc, { 0.0f, 0.0f, 0.0f }, dt);
    Require(cc.position.y > 0.05f, "jump leaves the ground");
}

// Level geometry: a quad in engine winding (clockwise from above in LH
// space), placed with offset + non-uniform scale like a level mesh entity.
void TestStaticMeshCollider()
{
    const std::vector<XMFLOAT3> vertices = {
        { -10.0f, 0.0f, -10.0f }, { 10.0f, 0.0f, -10.0f }, { 10.0f, 0.0f, 10.0f }, { -10.0f, 0.0f, 10.0f } };
    const std::vector<uint32_t> indices = { 0, 3, 2, 0, 2, 1 };
    std::string error;
    auto collider = SE::PhysicsWorld::BuildStaticMeshCollider(vertices, indices, &error);
    Require(collider != nullptr, "collider builds from triangles");

    SE::PhysicsWorld world;
    world.AddStaticMesh(collider, { 0.0f, 2.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 3.0f, 1.0f, 3.0f });
    SE::PhysicsWorld::RaycastHit hit;
    SE::Ray down;
    down.origin = { 25.0f, 10.0f, 0.0f }; // only inside the quad because of the 3x scale
    down.direction = { 0.0f, -1.0f, 0.0f };
    Require(world.Raycast(down, hit), "ray hits the scaled mesh from above");
    Require(hit.kind == SE::PhysicsWorld::RaycastHit::Kind::Mesh, "hit reports a mesh");
    Require(Near(hit.point.y, 2.0f, 1e-3f) && hit.normal.y > 0.99f, "mesh top face is at the placed height");

    SE::TransformComponent t;
    SE::RigidBodyComponent rb;
    rb.restitution = 0.0f;
    t.position = { 0.0f, 6.0f, 0.0f };
    world.AddSphere(&t, &rb, 0.5f);
    Simulate(world, 3.0f);
    Require(Near(t.position.y, 2.5f, 0.03f), "ball rests on the mesh");
}
}

int main()
{
    TestBallSettlesOnFloor();
    TestComponentEditsArePushed();
    TestTiltedBoxOrientation();
    TestRaycastReportsBodies();
    TestCharacterLandsAndWalks();
    TestStaticMeshCollider();
    std::puts("PhysicsWorldTests: all six groups passed");
    return 0;
}
