#include "Engine/Scene/SceneRuntime.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "Engine/Core/Logger.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

namespace SE {

namespace {

// Render meshes of a vehicle definition ("visual" block), resolved relative
// to the definition file. The physics side is read by LoadVehicleDesc.
struct VehicleVisual
{
    std::string                                      bodyMesh;
    std::array<std::string, VehicleDesc::WheelCount> wheelMeshes;   // modelled around the wheel center
    std::array<std::string, VehicleDesc::WheelCount> spindleMeshes; // same origin; empty when absent
};

bool LoadVehicleVisual(const std::string& path, VehicleVisual& out, std::string& error)
{
    namespace fs = std::filesystem;
    std::ifstream file(path);
    const nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if (!file || j.is_discarded())
    {
        error = "Cannot read " + path;
        return false;
    }
    const fs::path dir = fs::path(path).parent_path();
    auto resolve = [&dir](const nlohmann::json& rel) {
        return (dir / rel.get<std::string>()).generic_string();
    };
    static const char* k_wheelKeys[VehicleDesc::WheelCount] = { "fl", "fr", "rl", "rr" };
    try
    {
        const nlohmann::json& visual = j.at("visual");
        out.bodyMesh = resolve(visual.at("body"));
        for (int w = 0; w < VehicleDesc::WheelCount; ++w)
            out.wheelMeshes[w] = resolve(visual.at("wheels").at(k_wheelKeys[w]));
        if (visual.contains("spindles"))
            for (int w = 0; w < VehicleDesc::WheelCount; ++w)
                out.spindleMeshes[w] = resolve(visual.at("spindles").at(k_wheelKeys[w]));
    }
    catch (const nlohmann::json::exception& e)
    {
        error = path + ": " + e.what();
        return false;
    }
    return true;
}

} // namespace

uint32_t SceneRuntime::RequestVehicleSpawn(const std::string& vehiclePath,
                                           const DirectX::XMFLOAT3& groundPosition, float yawDeg)
{
    m_vehicleSpawnError.clear();
    if (m_pendingVehicle)
    {
        m_vehicleSpawnError = "a vehicle spawn is already in flight";
        return 0;
    }
    if (!m_assets)
    {
        m_vehicleSpawnError = "no scene runtime is available";
        return 0;
    }
    auto pending = std::make_unique<PendingVehicle>();
    VehicleVisual visual;
    if (!LoadVehicleDesc(vehiclePath, pending->desc, &m_vehicleSpawnError) ||
        !LoadVehicleVisual(vehiclePath, visual, m_vehicleSpawnError))
    {
        SE_LOG_ERROR("Vehicle spawn failed: %s", m_vehicleSpawnError.c_str());
        return 0;
    }
    pending->id = m_nextVehicleId++;
    pending->groundPosition = groundPosition;
    pending->yawDeg = yawDeg;

    const AssetCacheSnapshot cache = m_assets->CaptureSnapshot();
    bool started = pending->meshes[0].Start(visual.bodyMesh, cache);
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        started &= pending->meshes[1 + w].Start(visual.wheelMeshes[w], cache);
        if (!visual.spindleMeshes[w].empty())
            started &= pending->meshes[1 + VehicleDesc::WheelCount + w].Start(visual.spindleMeshes[w], cache);
    }
    if (!started)
    {
        m_vehicleSpawnError = "Could not start importing the meshes of " + vehiclePath;
        SE_LOG_ERROR("Vehicle spawn failed: %s", m_vehicleSpawnError.c_str());
        return 0;
    }
    const uint32_t id = pending->id;
    m_pendingVehicle = std::move(pending);
    return id;
}

SceneRuntime::VehicleSpawnState SceneRuntime::GetVehicleSpawnState() const
{
    if (m_pendingVehicle)
        return VehicleSpawnState::Loading;
    return m_vehicleSpawnError.empty() ? VehicleSpawnState::Idle : VehicleSpawnState::Failed;
}

SceneRuntime::VehicleInstance* SceneRuntime::FindVehicle(uint32_t id)
{
    for (const auto& vehicle : vehicles)
        if (vehicle->id == id)
            return vehicle.get();
    return nullptr;
}

const SceneRuntime::VehicleInstance* SceneRuntime::FindVehicle(uint32_t id) const
{
    for (const auto& vehicle : vehicles)
        if (vehicle->id == id)
            return vehicle.get();
    return nullptr;
}

void SceneRuntime::RemoveVehicles()
{
    for (const auto& instance : vehicles)
    {
        instance->vehicle->Shutdown();
        world.GetScene().DestroyEntity(instance->entity);
    }
    vehicles.clear();
    m_pendingVehicle.reset();
    m_vehicleSpawnError.clear();
}

void SceneRuntime::PumpVehicleSpawn()
{
    if (!m_pendingVehicle)
        return;
    PendingVehicle& pending = *m_pendingVehicle;
    bool complete = true;
    for (auto& op : pending.meshes)
    {
        if (op.GetState() == MeshAddState::Idle)
            continue;
        op.PumpUploads(m_renderer->GetDevice(), *m_assets);
        const MeshAddState state = op.GetState();
        if (state == MeshAddState::Failed)
        {
            m_vehicleSpawnError = op.GetPath() + ": " + op.GetError();
            SE_LOG_ERROR("Vehicle spawn failed: %s", m_vehicleSpawnError.c_str());
            m_pendingVehicle.reset();
            return;
        }
        complete &= state == MeshAddState::Complete;
    }
    if (!complete)
        return;

    auto instance = std::make_unique<VehicleInstance>();
    instance->id = pending.id;
    auto takePart = [&pending](size_t index, VehiclePart& part) {
        if (pending.meshes[index].GetState() != MeshAddState::Complete)
            return;
        std::unique_ptr<MeshAddOperation::Result> result = pending.meshes[index].TakeResult();
        part.mesh      = std::move(result->mesh);
        part.materials = std::move(result->materials);
        // Alpha-tested/transparent glass must be filtered out of the shadow passes
        // exactly like level meshes; vehicle parts are otherwise opaque casters.
        part.shadowSubMeshes = BuildShadowSubMeshes(part.materials);
    };
    takePart(0, instance->body);
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        takePart(1 + w, instance->wheels[w]);
        takePart(1 + VehicleDesc::WheelCount + w, instance->spindles[w]);
    }

    Entity* entity = world.GetScene().CreateEntity(pending.desc.name);
    instance->entity = entity->GetID();
    instance->transform = entity->AddComponent<TransformComponent>();
    auto* body3d = entity->AddComponent<RigidBodyComponent>();
    instance->transform->position = pending.groundPosition;
    instance->transform->eulerDeg = { 0.0f, pending.yawDeg, 0.0f };
    instance->vehicle = std::make_unique<Vehicle>();
    if (!instance->vehicle->Init(physicsWorld, pending.desc, instance->transform, body3d,
                                 &m_vehicleSpawnError))
    {
        SE_LOG_ERROR("Vehicle spawn failed: %s", m_vehicleSpawnError.c_str());
        world.GetScene().DestroyEntity(instance->entity);
        m_pendingVehicle.reset();
        return;
    }
    SE_LOG_INFO("Vehicle spawned: %s", pending.desc.name.c_str());
    vehicles.push_back(std::move(instance));
    m_pendingVehicle.reset();
}

} // namespace SE
