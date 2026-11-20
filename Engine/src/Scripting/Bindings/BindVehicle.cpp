#include <algorithm>
#include <cmath>
#include <string>
#include <DirectXMath.h>
#include "Engine/Scene/SceneRuntime.h"
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"
#include "Scripting/ScriptTypes.h"

using namespace DirectX;

namespace SE::Scripting {

namespace {

constexpr double k_radToDeg = 180.0 / 3.14159265358979323846;

SceneRuntime& RequireRuntime(ScriptContext& ctx, const char* api)
{
    if (!ctx.host.scene)
        Raise("%s is not available in this context (no scene runtime)", api);
    return *ctx.host.scene;
}

SceneRuntime::VehicleInstance& RequireVehicle(ScriptContext& ctx, const ScriptVehicle& handle,
                                              const char* api)
{
    SceneRuntime& scene = RequireRuntime(ctx, api);
    if (handle.generation != ctx.sceneGeneration)
        Raise("%s: this vehicle belongs to a scene that is no longer loaded", api);
    SceneRuntime::VehicleInstance* vehicle = scene.FindVehicle(handle.id);
    if (!vehicle)
    {
        const std::string& error = scene.GetVehicleSpawnError();
        if (scene.GetVehicleSpawnState() == SceneRuntime::VehicleSpawnState::Loading)
            Raise("%s: vehicle %u is still loading", api, handle.id);
        if (!error.empty())
            Raise("%s: vehicle %u is not available (%s)", api, handle.id, error.c_str());
        Raise("%s: vehicle %u is not available", api, handle.id);
    }
    return *vehicle;
}

bool IsReady(ScriptContext& ctx, const ScriptVehicle& handle, const char* api)
{
    SceneRuntime& scene = RequireRuntime(ctx, api);
    if (handle.generation != ctx.sceneGeneration)
        Raise("%s: this vehicle belongs to a scene that is no longer loaded", api);
    return scene.FindVehicle(handle.id) != nullptr;
}

double ClampValue(double value, double min, double max, const char* field)
{
    if (!std::isfinite(value))
        Raise("%s must be a finite number", field);
    return std::clamp(value, min, max);
}

ScriptVec3 Forward(const TransformComponent& transform)
{
    XMFLOAT3 forward;
    XMStoreFloat3(&forward, XMVector3Normalize(transform.GetWorldMatrix().r[2]));
    forward.y = 0.0f;
    const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (length < 1e-5f)
        return { 0.0, 0.0, 1.0 };
    return { forward.x / length, 0.0, forward.z / length };
}

} // namespace

void BindVehicle(ScriptApi& api, ScriptContext& ctx)
{
    api.Class<ScriptVehicle>("Vehicle",
        "A wheeled vehicle spawned at runtime (meshes load asynchronously; `ready`\n"
        "turns true once it can be driven). Vehicles are not saved with the level\n"
        "and are removed by the next scene load.",
        sol::no_constructor)
        .ReadOnly("ready", "boolean", "True once the meshes are loaded and the chassis exists.",
            [&ctx](const ScriptVehicle& self) {
                return IsReady(ctx, self, "fox.Vehicle.ready");
            })
        .ReadOnly("position", "fox.Vec3", "Chassis origin in world space.",
            [&ctx](const ScriptVehicle& self) {
                const auto& vehicle = RequireVehicle(ctx, self, "fox.Vehicle.position");
                XMFLOAT3 position;
                XMStoreFloat3(&position, vehicle.transform->GetWorldMatrix().r[3]);
                return FromFloat3(position);
            })
        .ReadOnly("forward", "fox.Vec3", "Unit forward direction on the ground plane.",
            [&ctx](const ScriptVehicle& self) {
                return Forward(*RequireVehicle(ctx, self, "fox.Vehicle.forward").transform);
            })
        .ReadOnly("yaw", "number", "Heading in degrees (yaw turns +Z toward +X).",
            [&ctx](const ScriptVehicle& self) {
                const ScriptVec3 forward = Forward(*RequireVehicle(ctx, self, "fox.Vehicle.yaw").transform);
                return std::atan2(forward.x, forward.z) * k_radToDeg;
            })
        .ReadOnly("speed", "number", "Signed speed along the chassis forward axis, m/s.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<double>(RequireVehicle(ctx, self, "fox.Vehicle.speed")
                                               .vehicle->GetTelemetry().speed);
            })
        .ReadOnly("rpm", "number", "Engine speed.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<double>(RequireVehicle(ctx, self, "fox.Vehicle.rpm")
                                               .vehicle->GetTelemetry().engineRpm);
            })
        .ReadOnly("limiter_rpm", "number", "Rev limiter engine speed from the definition.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<double>(RequireVehicle(ctx, self, "fox.Vehicle.limiter_rpm")
                                               .vehicle->GetDesc().engine.limiterRpm);
            })
        .ReadOnly("gear", "integer", "-1 reverse, 0 neutral, 1..gear_count forward.",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.gear").vehicle->GetTelemetry().gear;
            })
        .ReadOnly("gear_count", "integer", "Forward gears.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<int>(RequireVehicle(ctx, self, "fox.Vehicle.gear_count")
                                            .vehicle->GetDesc().gearbox.ratios.size());
            })
        .ReadOnly("shifting", "boolean", "True while the gearbox interrupts drive.",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.shifting").vehicle->GetTelemetry().shifting;
            })
        .ReadOnly("clutch", "number", "Clutch engagement 0..1.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<double>(RequireVehicle(ctx, self, "fox.Vehicle.clutch")
                                               .vehicle->GetTelemetry().clutch);
            })
        .ReadOnly("throttle", "number", "Effective throttle after limiter and traction control.",
            [&ctx](const ScriptVehicle& self) {
                return static_cast<double>(RequireVehicle(ctx, self, "fox.Vehicle.throttle")
                                               .vehicle->GetTelemetry().throttle);
            })
        .ReadOnly("traction_control_active", "boolean", "Traction control is cutting torque.",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.traction_control_active")
                    .vehicle->GetTelemetry().tractionControlActive;
            })
        .ReadOnly("abs_active", "boolean", "ABS is releasing brake pressure.",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.abs_active")
                    .vehicle->GetTelemetry().absActive;
            })
        .ReadOnly("acceleration", "fox.Vec3",
            "Chassis-space acceleration in m/s^2 (x right, y up, z forward).",
            [&ctx](const ScriptVehicle& self) {
                return FromFloat3(RequireVehicle(ctx, self, "fox.Vehicle.acceleration")
                                      .vehicle->GetTelemetry().acceleration);
            })
        .Property("automatic", "boolean", "Automatic gearbox (on by default).",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.automatic").vehicle->IsAutomatic();
            },
            [&ctx](const ScriptVehicle& self, bool value) {
                RequireVehicle(ctx, self, "fox.Vehicle.automatic").vehicle->SetAutomatic(value);
            })
        .Property("traction_control", "boolean", "Traction control (on by default).",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.traction_control")
                    .vehicle->IsTractionControlEnabled();
            },
            [&ctx](const ScriptVehicle& self, bool value) {
                RequireVehicle(ctx, self, "fox.Vehicle.traction_control").vehicle->SetTractionControl(value);
            })
        .Property("abs", "boolean", "Anti-lock brakes (on by default).",
            [&ctx](const ScriptVehicle& self) {
                return RequireVehicle(ctx, self, "fox.Vehicle.abs").vehicle->IsAbsEnabled();
            },
            [&ctx](const ScriptVehicle& self, bool value) {
                RequireVehicle(ctx, self, "fox.Vehicle.abs").vehicle->SetAbs(value);
            })
        .Method("set_input", "(throttle: number, brake: number, steer: number, handbrake?: number) -> nil",
            "Driver controls for the next physics step: pedals 0..1, steer -1..1\n"
            "(+ = right). Held until changed.",
            [&ctx](const ScriptVehicle& self, double throttle, double brake, double steer,
                   sol::optional<double> handbrake) {
                VehicleInput input;
                input.throttle  = static_cast<float>(ClampValue(throttle, 0.0, 1.0, "fox.Vehicle.set_input throttle"));
                input.brake     = static_cast<float>(ClampValue(brake, 0.0, 1.0, "fox.Vehicle.set_input brake"));
                input.steer     = static_cast<float>(ClampValue(steer, -1.0, 1.0, "fox.Vehicle.set_input steer"));
                input.handbrake = static_cast<float>(ClampValue(handbrake.value_or(0.0), 0.0, 1.0,
                                                                "fox.Vehicle.set_input handbrake"));
                RequireVehicle(ctx, self, "fox.Vehicle.set_input").vehicle->SetInput(input);
            })
        .Method("shift_up", "() -> nil", "Queues the next higher gear (forces manual).",
            [&ctx](const ScriptVehicle& self) {
                Vehicle& vehicle = *RequireVehicle(ctx, self, "fox.Vehicle.shift_up").vehicle;
                vehicle.SetAutomatic(false);
                vehicle.ShiftUp();
            })
        .Method("shift_down", "() -> nil", "Queues the next lower gear (forces manual).",
            [&ctx](const ScriptVehicle& self) {
                Vehicle& vehicle = *RequireVehicle(ctx, self, "fox.Vehicle.shift_down").vehicle;
                vehicle.SetAutomatic(false);
                vehicle.ShiftDown();
            })
        .Method("select_gear", "(gear: integer) -> nil", "-1 reverse, 0 neutral, 1..gear_count.",
            [&ctx](const ScriptVehicle& self, lua_Integer gear) {
                RequireVehicle(ctx, self, "fox.Vehicle.select_gear")
                    .vehicle->SelectGear(static_cast<int>(std::clamp<lua_Integer>(gear, -1, 99)));
            })
        .Method("reset", "(position?: fox.Vec3, yaw?: number) -> nil",
            "Teleports upright to the position and heading (default: current) and\n"
            "clears all dynamic state (recovery after a crash or flip).",
            [&ctx](const ScriptVehicle& self, sol::optional<ScriptVec3> position,
                   sol::optional<double> yaw) {
                SceneRuntime::VehicleInstance& instance = RequireVehicle(ctx, self, "fox.Vehicle.reset");
                XMFLOAT3 ground = instance.transform->position;
                if (position)
                    ground = ToFloat3(*position);
                const float heading = static_cast<float>(yaw.value_or(instance.transform->eulerDeg.y));
                instance.vehicle->Reset(ground, heading);
            })
        .Operator(sol::meta_function::to_string, "",
            [&ctx](const ScriptVehicle& self) {
                SceneRuntime& scene = RequireRuntime(ctx, "fox.Vehicle");
                const SceneRuntime::VehicleInstance* vehicle =
                    self.generation == ctx.sceneGeneration ? scene.FindVehicle(self.id) : nullptr;
                if (!vehicle)
                    return "fox.Vehicle " + std::to_string(self.id) + " (not ready)";
                return "fox.Vehicle '" + vehicle->vehicle->GetDesc().name + "'";
            });

    api.Module("vehicle",
        "Runtime-spawned wheeled vehicles. The definition JSON holds the physics\n"
        "(chassis, axles, engine, gearbox) and a \"visual\" block with the body and\n"
        "wheel meshes. Spawning imports meshes asynchronously; check `ready` before\n"
        "driving. Vehicles are not saved and are removed by the next scene load.")
        .Function("spawn", "(path: string, position?: fox.Vec3, yaw?: number) -> fox.Vehicle",
            "Starts spawning a vehicle definition (\"Assets/Vehicles/Car/vehicle.json\")\n"
            "with its wheels on the position (default origin). One spawn may be in\n"
            "flight at a time; the returned handle is usable once `ready` is true.",
            [&ctx](const std::string& path, sol::optional<ScriptVec3> position, sol::optional<double> yaw) {
                SceneRuntime& scene = RequireRuntime(ctx, "fox.vehicle.spawn");
                if (!scene.HasScene())
                    Raise("fox.vehicle.spawn: no scene is loaded");
                const XMFLOAT3 ground = position ? ToFloat3(*position) : XMFLOAT3 { 0.0f, 0.0f, 0.0f };
                const uint32_t id = scene.RequestVehicleSpawn(path, ground,
                                                              static_cast<float>(yaw.value_or(0.0)));
                if (id == 0)
                    Raise("fox.vehicle.spawn: %s", scene.GetVehicleSpawnError().c_str());
                return ScriptVehicle { id, ctx.sceneGeneration };
            })
        .Function("remove_all", "() -> nil", "Removes every spawned vehicle.",
            [&ctx] { RequireRuntime(ctx, "fox.vehicle.remove_all").RemoveVehicles(); })
        .Function("count", "() -> integer", "Vehicles that finished loading.",
            [&ctx] { return static_cast<int>(RequireRuntime(ctx, "fox.vehicle.count").vehicles.size()); });
}

} // namespace SE::Scripting
