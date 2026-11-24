#include "EditorGizmo.h"

#include "Engine/Scene/SceneRuntime.h"
#include <ImGuizmo.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace Editor {

namespace {

// --- Transform snapshot (undo) ------------------------------------------------

struct Snapshot
{
    XMFLOAT3 position = { 0, 0, 0 };
    XMFLOAT3 euler    = { 0, 0, 0 };
    XMFLOAT3 scale    = { 1, 1, 1 };
};

bool operator==(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool HasFullTransform(Selection::Kind kind)
{
    return kind == Selection::Kind::Mesh || kind == Selection::Kind::Empty ||
           kind == Selection::Kind::Actor;
}

// Returns the gizmo target's world position (and full TRS for meshes and empties).
bool CaptureSnapshot(SE::SceneRuntime& scene, const Selection& sel, Snapshot& out)
{
    switch (sel.kind)
    {
    case Selection::Kind::Mesh:
    {
        if (sel.index >= (int)scene.loadedMeshes.size()) return false;
        const auto* t = scene.loadedMeshes[sel.index].transform;
        if (!t) return false;
        out = { t->position, t->eulerDeg, t->scale };
        return true;
    }
    case Selection::Kind::Empty:
    {
        if (sel.index >= (int)scene.emptyEntities.size()) return false;
        const auto* t = scene.emptyEntities[sel.index].transform;
        if (!t) return false;
        out = { t->position, t->eulerDeg, t->scale };
        return true;
    }
    case Selection::Kind::Actor:
    {
        if (sel.index >= (int)scene.actors.size()) return false;
        const auto* t = scene.actors[sel.index].transform;
        if (!t) return false;
        out = { t->position, t->eulerDeg, t->scale };
        return true;
    }
    case Selection::Kind::PointLight:
        if (sel.index >= scene.lights.numLights) return false;
        out.position = scene.lights.lights[sel.index].position;
        return true;
    case Selection::Kind::SpotLight:
        out.position = scene.spotLight.position;
        return true;
    case Selection::Kind::Emitter:
        if (sel.index >= (int)scene.particleSystems.size()) return false;
        out.position = scene.particleSystems[sel.index]->config.position;
        return true;
    case Selection::Kind::Object:
    {
        if (sel.index >= (int)scene.sceneObjects.size()) return false;
        const auto& p = scene.sceneObjects[sel.index].def.position;
        out.position = { p[0], p[1], p[2] };
        return true;
    }
    default:
        return false;
    }
}

void ApplySnapshot(SE::SceneRuntime& scene, const Selection& sel, const Snapshot& s)
{
    switch (sel.kind)
    {
    case Selection::Kind::Mesh:
    {
        if (sel.index >= (int)scene.loadedMeshes.size()) return;
        auto& loaded = scene.loadedMeshes[sel.index];
        if (!loaded.transform) return;
        loaded.transform->position = s.position;
        loaded.transform->eulerDeg = s.euler;
        loaded.transform->scale    = s.scale;
        loaded.def.position = { s.position.x, s.position.y, s.position.z };
        loaded.def.rotation = { s.euler.x, s.euler.y, s.euler.z };
        loaded.def.scale    = { s.scale.x, s.scale.y, s.scale.z };
        return;
    }
    case Selection::Kind::Empty:
    {
        if (sel.index >= (int)scene.emptyEntities.size()) return;
        auto& empty = scene.emptyEntities[sel.index];
        if (!empty.transform) return;
        empty.transform->position = s.position;
        empty.transform->eulerDeg = s.euler;
        empty.transform->scale    = s.scale;
        empty.def.position = { s.position.x, s.position.y, s.position.z };
        empty.def.rotation = { s.euler.x, s.euler.y, s.euler.z };
        empty.def.scale    = { s.scale.x, s.scale.y, s.scale.z };
        return;
    }
    case Selection::Kind::Actor:
    {
        if (sel.index >= (int)scene.actors.size()) return;
        auto* t = scene.actors[sel.index].transform;
        if (!t) return;
        t->position = s.position;
        t->eulerDeg = s.euler;
        t->scale    = s.scale;
        return;
    }
    case Selection::Kind::PointLight:
        if (sel.index >= scene.lights.numLights) return;
        scene.lights.lights[sel.index].position = s.position;
        return;
    case Selection::Kind::SpotLight:
        scene.spotLight.position = s.position;
        return;
    case Selection::Kind::Emitter:
        if (sel.index >= (int)scene.particleSystems.size()) return;
        scene.particleSystems[sel.index]->config.position = s.position;
        return;
    case Selection::Kind::Object:
        if (sel.index >= (int)scene.sceneObjects.size()) return;
        scene.sceneObjects[sel.index].def.position =
            { s.position.x, s.position.y, s.position.z };
        return;
    default:
        return;
    }
}

// --- Matrix helpers -------------------------------------------------------------

void SnapshotToMatrix(const Snapshot& s, XMFLOAT4X4& out)
{
    XMStoreFloat4x4(&out,
        XMMatrixScaling(s.scale.x, s.scale.y, s.scale.z) *
        XMMatrixRotationRollPitchYaw(XMConvertToRadians(s.euler.x),
                                     XMConvertToRadians(s.euler.y),
                                     XMConvertToRadians(s.euler.z)) *
        XMMatrixTranslation(s.position.x, s.position.y, s.position.z));
}

// Decomposes an SRT matrix into the XMMatrixRotationRollPitchYaw euler
// convention used by TransformComponent (pitch/yaw/roll in degrees).
void MatrixToSnapshot(const XMFLOAT4X4& m, Snapshot& out)
{
    out.position = { m.m[3][0], m.m[3][1], m.m[3][2] };

    XMVECTOR rows[3];
    float    len[3];
    for (int i = 0; i < 3; ++i)
    {
        rows[i] = XMVectorSet(m.m[i][0], m.m[i][1], m.m[i][2], 0.0f);
        len[i]  = XMVectorGetX(XMVector3Length(rows[i]));
    }
    out.scale = { len[0], len[1], len[2] };

    float r[3][3];
    for (int i = 0; i < 3; ++i)
    {
        const float inv = (len[i] > 1e-6f) ? 1.0f / len[i] : 0.0f;
        for (int j = 0; j < 3; ++j)
            r[i][j] = m.m[i][j] * inv;
    }

    // R = Rz(roll) * Rx(pitch) * Ry(yaw) (row-vector convention):
    //   r[2][1] = -sin(pitch), r[2][0] = cos(pitch)sin(yaw),
    //   r[2][2] = cos(pitch)cos(yaw), r[0][1] = sin(roll)cos(pitch),
    //   r[1][1] = cos(roll)cos(pitch)
    const float sinPitch = std::clamp(-r[2][1], -1.0f, 1.0f);
    const float pitch    = asinf(sinPitch);
    float yaw, roll;
    if (fabsf(cosf(pitch)) > 1e-4f)
    {
        yaw  = atan2f(r[2][0], r[2][2]);
        roll = atan2f(r[0][1], r[1][1]);
    }
    else // gimbal lock: fold roll into yaw
    {
        yaw  = atan2f(-r[0][2], r[0][0]);
        roll = 0.0f;
    }
    out.euler = { XMConvertToDegrees(pitch), XMConvertToDegrees(yaw),
                  XMConvertToDegrees(roll) };
}

void PushTransformUndo(SE::SceneRuntime& scene, UndoStack& undo, Selection sel,
                       const Snapshot& before, const Snapshot& after)
{
    if (before.position == after.position && before.euler == after.euler &&
        before.scale == after.scale)
        return;
    undo.Push({ "Transform",
                [&scene, sel, before] { ApplySnapshot(scene, sel, before); },
                [&scene, sel, after]  { ApplySnapshot(scene, sel, after);  } });
}

// --- Picking ---------------------------------------------------------------------

struct PickRay
{
    XMVECTOR origin;
    XMVECTOR dir; // normalized, world space
};

PickRay BuildPickRay(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                     ImVec2 mousePos)
{
    const float w = imageMax.x - imageMin.x;
    const float h = imageMax.y - imageMin.y;
    const float ndcX = ((mousePos.x - imageMin.x) / w) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mousePos.y - imageMin.y) / h) * 2.0f;

    XMMATRIX view  = scene.camera->GetViewMatrix();
    XMMATRIX proj  = scene.camera->GetProjectionMatrix(w / h);
    XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);

    XMVECTOR pNear = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);
    XMVECTOR pFar  = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);
    return { pNear, XMVector3Normalize(XMVectorSubtract(pFar, pNear)) };
}

// tOut is the entry distance (0 when the origin is inside); tExitOut the exit.
bool RayAABB(XMVECTOR origin, XMVECTOR dir, const SE::AABB& box, float& tOut,
             float* tExitOut = nullptr)
{
    const float ro[3] = { XMVectorGetX(origin), XMVectorGetY(origin), XMVectorGetZ(origin) };
    const float rd[3] = { XMVectorGetX(dir),    XMVectorGetY(dir),    XMVectorGetZ(dir) };
    const float bmin[3] = { box.min.x, box.min.y, box.min.z };
    const float bmax[3] = { box.max.x, box.max.y, box.max.z };

    float tMin = 0.0f, tMax = FLT_MAX;
    for (int i = 0; i < 3; ++i)
    {
        if (fabsf(rd[i]) < 1e-8f)
        {
            if (ro[i] < bmin[i] || ro[i] > bmax[i])
                return false;
            continue;
        }
        float t1 = (bmin[i] - ro[i]) / rd[i];
        float t2 = (bmax[i] - ro[i]) / rd[i];
        if (t1 > t2) std::swap(t1, t2);
        tMin = (std::max)(tMin, t1);
        tMax = (std::min)(tMax, t2);
        if (tMin > tMax)
            return false;
    }
    tOut = tMin;
    if (tExitOut)
        *tExitOut = tMax;
    return true;
}

// World-space hit distance for a mesh drawn with transform, or FLT_MAX on
// miss. The ray is tested in mesh-local space against the mesh AABB.
float HitDistanceMesh(const PickRay& ray, const SE::Mesh* mesh, const SE::TransformComponent* transform)
{
    if (!mesh || !transform)
        return FLT_MAX;
    XMMATRIX world    = transform->GetWorldMatrix();
    XMMATRIX invWorld = XMMatrixInverse(nullptr, world);
    XMVECTOR lo = XMVector3TransformCoord(ray.origin, invWorld);
    XMVECTOR ld = XMVector3TransformNormal(ray.dir, invWorld);
    float t, tExit;
    if (!RayAABB(lo, ld, mesh->GetBounds(), t, &tExit))
        return FLT_MAX;
    // Camera inside the mesh bounds (typical for level geometry): score the
    // exit distance so entities inside the AABB stay clickable and the mesh
    // acts as the fallback hit.
    if (t <= 0.0f)
        t = tExit;
    XMVECTOR hitWorld = XMVector3TransformCoord(XMVectorAdd(lo, XMVectorScale(ld, t)), world);
    return XMVectorGetX(XMVector3Length(XMVectorSubtract(hitWorld, ray.origin)));
}

// Nearest hit over an actor's mesh parts; actors without meshes pick through
// a proxy box at the root, like point lights.
float HitDistanceActor(const PickRay& ray, const SE::SceneRuntime::LoadedActor& actor)
{
    float best = FLT_MAX;
    bool hasMesh = false;
    for (const auto& part : actor.parts)
        if (part.mesh)
        {
            hasMesh = true;
            best = (std::min)(best, HitDistanceMesh(ray, part.mesh.get(), part.transform));
        }
    if (!hasMesh && actor.transform)
    {
        XMFLOAT3 p;
        XMStoreFloat3(&p, actor.transform->GetWorldMatrix().r[3]);
        float t;
        if (RayAABB(ray.origin, ray.dir, SE::AABB::FromCenterExtents(p, { 0.5f, 0.5f, 0.5f }), t))
            best = t;
    }
    return best;
}

float HitDistanceObject(const PickRay& ray, const SE::SceneRuntime::LoadedObject& lo)
{
    using Type = SE::SceneDescriptor::SceneObject::Type;
    const auto& d = lo.def;
    SE::AABB box;
    if (d.type == Type::Sphere)
        box = SE::AABB::FromCenterExtents({ d.position[0], d.position[1], d.position[2] },
                                          { d.radius, d.radius, d.radius });
    else
        box = SE::AABB::FromCenterExtents({ d.position[0], d.position[1], d.position[2] },
                                          { d.halfSizeX, 0.05f, d.halfSizeZ });
    float t;
    return RayAABB(ray.origin, ray.dir, box, t) ? t : FLT_MAX;
}

} // namespace

namespace {

struct DragState
{
    bool      wasUsing = false;
    Snapshot  before;
    Selection sel;
};
DragState s_drag;

float SnapStep(GizmoOp op, const GizmoSettings& settings)
{
    switch (op)
    {
    case GizmoOp::Translate: return settings.translationSnap ? settings.translationStep : 0.0f;
    case GizmoOp::Rotate: return settings.rotationSnap ? settings.rotationStep : 0.0f;
    case GizmoOp::Scale: return settings.scaleSnap ? settings.scaleStep : 0.0f;
    default: return 0.0f;
    }
}

// Runs ImGuizmo over the viewport rect and returns true when the matrix
// was modified this frame.
bool RunManipulate(SE::SceneRuntime& scene, GizmoOp op, const GizmoSettings& settings,
                   const Snapshot& current, ImVec2 imageMin, ImVec2 imageMax, XMFLOAT4X4& matrix)
{
    ImGuizmo::SetOrthographic(scene.camera->orthographic);
    ImGuizmo::SetDrawlist();
    const float w = imageMax.x - imageMin.x;
    const float h = imageMax.y - imageMin.y;
    ImGuizmo::SetRect(imageMin.x, imageMin.y, w, h);
    ImGuizmo::Enable(!ImGui::GetIO().KeyAlt); // Alt belongs to the camera

    // Unjittered matrices; jittered ones make the gizmo shimmer under TAA/FSR.
    XMFLOAT4X4 view, proj;
    XMStoreFloat4x4(&view, scene.camera->GetViewMatrix());
    XMStoreFloat4x4(&proj, scene.camera->GetProjectionMatrix(w / h));
    SnapshotToMatrix(current, matrix);

    const ImGuizmo::OPERATION operation =
        op == GizmoOp::Rotate ? ImGuizmo::ROTATE :
        op == GizmoOp::Scale  ? ImGuizmo::SCALE  : ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode = settings.localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const float step = SnapStep(op, settings);
    const float snap[3] = { step, step, step };
    return ImGuizmo::Manipulate(&view.m[0][0], &proj.m[0][0], operation, mode,
                                &matrix.m[0][0], nullptr, step > 0.0f ? snap : nullptr);
}

void ApplyManipulation(SE::SceneRuntime& scene, const Selection& sel,
                       const XMFLOAT4X4& matrix, const Snapshot& current)
{
    Snapshot edited;
    MatrixToSnapshot(matrix, edited);
    if (!HasFullTransform(sel.kind)) // keep untouched components exact
    {
        edited.euler = current.euler;
        edited.scale = current.scale;
    }
    ApplySnapshot(scene, sel, edited);
}

// Captures the pre-drag state when a manipulation starts and pushes an undo
// entry when it ends.
void TrackDragUndo(SE::SceneRuntime& scene, UndoStack& undo, const Selection& sel,
                   bool usingNow, const Snapshot& current)
{
    if (usingNow && !s_drag.wasUsing)
    {
        s_drag.before = current;
        s_drag.sel    = sel;
    }
    if (!usingNow && s_drag.wasUsing)
    {
        Snapshot after;
        if (CaptureSnapshot(scene, s_drag.sel, after))
            PushTransformUndo(scene, undo, s_drag.sel, s_drag.before, after);
    }
    s_drag.wasUsing = usingNow;
}

} // namespace

bool DrawGizmo(SE::SceneRuntime& scene, const Selection& sel, UndoStack& undo,
               GizmoOp op, const GizmoSettings& settings, ImVec2 imageMin, ImVec2 imageMax)
{
    Snapshot current;
    if (op == GizmoOp::None || !CaptureSnapshot(scene, sel, current))
    {
        s_drag.wasUsing = false;
        return false;
    }

    // Point-target selections only support translation.
    if (!HasFullTransform(sel.kind) && op != GizmoOp::Translate)
        op = GizmoOp::Translate;

    XMFLOAT4X4 matrix;
    const bool changed  = RunManipulate(scene, op, settings, current, imageMin, imageMax, matrix);
    const bool usingNow = ImGuizmo::IsUsing();
    if (changed)
        ApplyManipulation(scene, sel, matrix, current);
    TrackDragUndo(scene, undo, sel, usingNow, current);

    return ImGuizmo::IsOver() || usingNow;
}

namespace {

// Grows [wmin, wmax] by the mesh AABB's 8 corners in world space.
void AccumulateBounds(const SE::Mesh& mesh, const SE::TransformComponent& transform,
                      XMVECTOR& wmin, XMVECTOR& wmax)
{
    const SE::AABB& b = mesh.GetBounds();
    XMMATRIX world = transform.GetWorldMatrix();
    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR corner = XMVectorSet((i & 1) ? b.max.x : b.min.x,
                                      (i & 2) ? b.max.y : b.min.y,
                                      (i & 4) ? b.max.z : b.min.z, 1.0f);
        corner = XMVector3TransformCoord(corner, world);
        wmin = XMVectorMin(wmin, corner);
        wmax = XMVectorMax(wmax, corner);
    }
}

void StoreSphere(XMVECTOR wmin, XMVECTOR wmax, XMFLOAT3& outCenter, float& outRadius)
{
    XMStoreFloat3(&outCenter, XMVectorScale(XMVectorAdd(wmin, wmax), 0.5f));
    outRadius = 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(wmax, wmin)));
}

} // namespace

bool GetSelectionBounds(SE::SceneRuntime& scene, const Selection& sel,
                        XMFLOAT3& outCenter, float& outRadius)
{
    Snapshot snap;
    if (!CaptureSnapshot(scene, sel, snap))
        return false;
    outCenter = snap.position;
    outRadius = 2.0f; // sensible framing for point targets (lights, emitters)

    switch (sel.kind)
    {
    case Selection::Kind::Mesh:
    {
        const auto& loaded = scene.loadedMeshes[sel.index];
        if (loaded.mesh)
        {
            XMVECTOR wmin = XMVectorReplicate(FLT_MAX), wmax = XMVectorReplicate(-FLT_MAX);
            AccumulateBounds(*loaded.mesh, *loaded.transform, wmin, wmax);
            StoreSphere(wmin, wmax, outCenter, outRadius);
        }
        break;
    }
    case Selection::Kind::Actor:
    {
        XMVECTOR wmin = XMVectorReplicate(FLT_MAX), wmax = XMVectorReplicate(-FLT_MAX);
        bool any = false;
        for (const auto& part : scene.actors[sel.index].parts)
            if (part.mesh && part.transform)
            {
                AccumulateBounds(*part.mesh, *part.transform, wmin, wmax);
                any = true;
            }
        if (any)
            StoreSphere(wmin, wmax, outCenter, outRadius);
        break;
    }
    case Selection::Kind::Emitter:
        outRadius = (std::max)(
            scene.particleSystems[sel.index]->config.spawnRadius, 2.0f);
        break;
    case Selection::Kind::Object:
    {
        const auto& d = scene.sceneObjects[sel.index].def;
        using Type = SE::SceneDescriptor::SceneObject::Type;
        outRadius = d.type == Type::Sphere ? d.radius
                                           : (std::max)(d.halfSizeX, d.halfSizeZ);
        break;
    }
    default:
        break;
    }
    outRadius = (std::max)(outRadius, 0.5f);
    return true;
}

Selection PickScene(SE::SceneRuntime& scene, ImVec2 imageMin, ImVec2 imageMax,
                    ImVec2 mousePos)
{
    Selection result;
    if (!scene.HasScene())
        return result;

    const PickRay ray = BuildPickRay(scene, imageMin, imageMax, mousePos);
    float best = FLT_MAX;

    for (int i = 0; i < (int)scene.loadedMeshes.size(); ++i)
    {
        const auto& loaded = scene.loadedMeshes[i];
        // A collider-only mesh is drawn (as wireframe) only with Show Colliders on.
        if (!loaded.def.visible && !(scene.showColliders && loaded.collider))
            continue;
        const float d = HitDistanceMesh(ray, loaded.mesh.get(), loaded.transform);
        if (d < best)
        {
            best = d;
            result.Set(Selection::Kind::Mesh, i);
        }
    }
    for (int i = 0; i < (int)scene.sceneObjects.size(); ++i)
    {
        const float d = HitDistanceObject(ray, scene.sceneObjects[i]);
        if (d < best)
        {
            best = d;
            result.Set(Selection::Kind::Object, i);
        }
    }
    for (int i = 0; i < (int)scene.actors.size(); ++i)
    {
        const float d = HitDistanceActor(ray, scene.actors[i]);
        if (d < best)
        {
            best = d;
            result.Set(Selection::Kind::Actor, i);
        }
    }
    // Invisible helpers last: small proxy boxes for point lights and emitters.
    for (int i = 0; i < scene.lights.numLights; ++i)
    {
        const auto& p = scene.lights.lights[i].position;
        SE::AABB box = SE::AABB::FromCenterExtents(p, { 0.5f, 0.5f, 0.5f });
        float t;
        if (RayAABB(ray.origin, ray.dir, box, t) && t < best)
        {
            best = t;
            result.Set(Selection::Kind::PointLight, i);
        }
    }
    for (int i = 0; i < (int)scene.particleSystems.size(); ++i)
    {
        const auto& p = scene.particleSystems[i]->config.position;
        SE::AABB box = SE::AABB::FromCenterExtents(p, { 0.5f, 0.5f, 0.5f });
        float t;
        if (RayAABB(ray.origin, ray.dir, box, t) && t < best)
        {
            best = t;
            result.Set(Selection::Kind::Emitter, i);
        }
    }
    return result;
}

} // namespace Editor
