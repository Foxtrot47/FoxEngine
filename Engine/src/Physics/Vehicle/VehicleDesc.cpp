#include "Engine/Physics/Vehicle/VehicleDesc.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <nlohmann/json.hpp>

namespace SE {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

bool Fail(std::string* error, const std::string& message)
{
    if (error) *error = message;
    return false;
}

DirectX::XMFLOAT3 ReadVec3(const json& j, const char* key, DirectX::XMFLOAT3 fallback)
{
    if (!j.contains(key)) return fallback;
    const auto& a = j.at(key);
    return { a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>() };
}

void ReadAxle(const json& j, VehicleDesc::Axle& axle)
{
    if (j.contains("suspension"))
    {
        const json& s = j.at("suspension");
        auto& o = axle.suspension;
        o.springRate       = s.value("springRate", o.springRate);
        o.bumpDamping      = s.value("bumpDamping", o.bumpDamping);
        o.reboundDamping   = s.value("reboundDamping", o.reboundDamping);
        o.bumpTravel       = s.value("bumpTravel", o.bumpTravel);
        o.droopTravel      = s.value("droopTravel", o.droopTravel);
        o.bumpStopRate     = s.value("bumpStopRate", o.bumpStopRate);
        o.antiRollRate     = s.value("antiRollRate", o.antiRollRate);
        o.rollCenterHeight = s.value("rollCenterHeight", o.rollCenterHeight);
    }
    axle.relaxationLengthLat = j.value("relaxationLengthLat", axle.relaxationLengthLat);
    axle.camberDeg        = j.value("camberDeg", axle.camberDeg);
    axle.wheelInertia     = j.value("wheelInertia", axle.wheelInertia);
    axle.maxBrakeTorque   = j.value("maxBrakeTorque", axle.maxBrakeTorque);
    axle.handbrakeTorque  = j.value("handbrakeTorque", axle.handbrakeTorque);
    axle.maxSteerAngleDeg = j.value("maxSteerAngleDeg", axle.maxSteerAngleDeg);
    axle.ackermann        = j.value("ackermann", axle.ackermann);
    axle.driven           = j.value("driven", axle.driven);
}

bool ReadJson(const fs::path& path, json& out, std::string* error)
{
    std::ifstream file(path);
    if (!file)
        return Fail(error, "Cannot open " + path.string());
    out = json::parse(file, nullptr, false);
    if (out.is_discarded())
        return Fail(error, "Invalid JSON in " + path.string());
    return true;
}

// Reader for the generated tires.json/aero.json: every key is required,
// unknown keys are errors. Only the first failure is kept.
class StrictReader
{
public:
    explicit StrictReader(std::string file) : m_file(std::move(file)) {}

    bool Failed() const { return !m_error.empty(); }
    const std::string& Error() const { return m_error; }

    void Fail(const std::string& at, const std::string& message)
    {
        if (m_error.empty())
            m_error = m_file + ": " + at + ": " + message;
    }

    static std::string Join(const std::string& at, const std::string& key)
    {
        return at.empty() ? key : at + "." + key;
    }

    const json& Object(const json& parent, const std::string& at, const char* key,
                       std::initializer_list<const char*> keys)
    {
        const json& o = Member(parent, at, key);
        CheckObject(o, Join(at, key), keys);
        return o;
    }

    void CheckObject(const json& o, const std::string& at, std::initializer_list<const char*> keys)
    {
        if (!o.is_object())
        {
            Fail(at.empty() ? "(root)" : at, "expected an object");
            return;
        }
        for (const auto& item : o.items())
            if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return item.key() == k; }))
                Fail(Join(at, item.key()), "unknown key");
        for (const char* k : keys)
            if (!o.contains(k))
                Fail(Join(at, k), "missing");
    }

    const json& Member(const json& parent, const std::string& at, const char* key)
    {
        static const json k_null;
        if (!parent.is_object() || !parent.contains(key))
        {
            Fail(Join(at, key), "missing");
            return k_null;
        }
        return parent.at(key);
    }

    float Number(const json& parent, const std::string& at, const char* key)
    {
        const json& v = Member(parent, at, key);
        if (v.is_number())
            return v.get<float>();
        if (!v.is_null())
            Fail(Join(at, key), "expected a number");
        return 0.0f;
    }

    std::string String(const json& parent, const std::string& at, const char* key)
    {
        const json& v = Member(parent, at, key);
        if (v.is_string())
            return v.get<std::string>();
        if (!v.is_null())
            Fail(Join(at, key), "expected a string");
        return {};
    }

    // `count` numbers, or at least `count` when `minimum`.
    std::vector<float> Numbers(const json& parent, const std::string& at, const char* key, size_t count,
                               bool minimum = false)
    {
        const json& v = Member(parent, at, key);
        std::vector<float> out;
        if (v.is_null())
            return std::vector<float>(count, 0.0f);
        const bool sizeOk = v.is_array() && (minimum ? v.size() >= count : v.size() == count);
        if (!sizeOk)
        {
            Fail(Join(at, key), (minimum ? "expected an array of at least " : "expected an array of ") +
                                    std::to_string(count) + " numbers");
            return std::vector<float>(count, 0.0f);
        }
        for (const json& e : v)
        {
            if (!e.is_number())
            {
                Fail(Join(at, key), "expected numbers");
                return std::vector<float>(count, 0.0f);
            }
            out.push_back(e.get<float>());
        }
        return out;
    }

    template <size_t N>
    void Numbers(const json& parent, const std::string& at, const char* key, float (&out)[N])
    {
        const std::vector<float> values = Numbers(parent, at, key, N);
        std::copy(values.begin(), values.begin() + N, out);
    }

    DirectX::XMFLOAT3 Vec3(const json& parent, const std::string& at, const char* key)
    {
        const std::vector<float> v = Numbers(parent, at, key, 3);
        return { v[0], v[1], v[2] };
    }

    void Header(const json& root, const char* format)
    {
        if (String(root, "", "format") != format)
            Fail("format", std::string("expected \"") + format + "\"");
        const json& version = Member(root, "", "version");
        if (!version.is_number_integer() || version.get<int>() != 1)
            Fail("version", "expected 1");
        String(root, "", "source");
    }

    void Positive(float value, const std::string& at)
    {
        if (!(value > 0.0f))
            Fail(at, "must be positive");
    }

private:
    std::string m_file;
    std::string m_error;
};

bool ReadTireCurve(StrictReader& r, const json& curves, const std::string& name, const std::string& at,
                   TireCurve& out)
{
    if (!curves.is_object() || !curves.contains(name))
    {
        r.Fail(at, "unknown curve \"" + name + "\"");
        return false;
    }
    const std::string curveAt = StrictReader::Join("curves", name);
    const json& c = curves.at(name);
    r.CheckObject(c, curveAt, { "step", "dropoff", "data" });
    out.step    = r.Number(c, curveAt, "step");
    out.dropoff = r.Number(c, curveAt, "dropoff");
    out.data    = r.Numbers(c, curveAt, "data", 2, true);
    r.Positive(out.step, curveAt + ".step");
    if (*std::max_element(out.data.begin(), out.data.end()) <= 0.0f)
        r.Fail(curveAt + ".data", "needs a positive sample");
    return !r.Failed();
}

void ReadTireAxis(StrictReader& r, const json& axle, const std::string& at, const char* key,
                  TireAxisParams& out)
{
    const bool lateral = std::string(key) == "lateral";
    const json& a = lateral
        ? r.Object(axle, at, key, { "mu", "loadSensitivity", "peakSlip", "curve" })
        : r.Object(axle, at, key, { "mu", "loadSensitivity", "peakSlip", "tractionCurve", "brakingCurve" });
    const std::string axisAt = StrictReader::Join(at, key);
    out.mu = r.Number(a, axisAt, "mu");
    r.Numbers(a, axisAt, "loadSensitivity", out.loadSens);
    r.Numbers(a, axisAt, "peakSlip", out.peakSlip);
    r.Positive(out.mu, axisAt + ".mu");
    r.Positive(out.loadSens[2], axisAt + ".loadSensitivity[2]");
    r.Positive(out.peakSlip[0], axisAt + ".peakSlip[0]");
    r.Positive(out.peakSlip[1], axisAt + ".peakSlip[1]");
    r.Positive(out.peakSlip[2], axisAt + ".peakSlip[2]");
}

void ReadTireAxle(StrictReader& r, const json& root, const char* key, TireParams& out)
{
    const json& a = r.Object(root, "", key, { "lateral", "longitudinal", "speedEffects", "camber",
                                              "rollingResistance", "verticalStiffness" });
    const std::string at = key;
    ReadTireAxis(r, a, at, "lateral", out.lateral);
    ReadTireAxis(r, a, at, "longitudinal", out.longitudinal);
    static const json k_noCurves;
    const json& curves = root.contains("curves") ? root.at("curves") : k_noCurves;
    if (a.contains("lateral"))
        ReadTireCurve(r, curves, r.String(a.at("lateral"), at + ".lateral", "curve"),
                      at + ".lateral.curve", out.lateralCurve);
    if (a.contains("longitudinal"))
    {
        const json& l = a.at("longitudinal");
        ReadTireCurve(r, curves, r.String(l, at + ".longitudinal", "tractionCurve"),
                      at + ".longitudinal.tractionCurve", out.tractionCurve);
        ReadTireCurve(r, curves, r.String(l, at + ".longitudinal", "brakingCurve"),
                      at + ".longitudinal.brakingCurve", out.brakingCurve);
    }
    float speed[2] = {}, camber[3] = {};
    r.Numbers(a, at, "speedEffects", speed);
    r.Numbers(a, at, "camber", camber);
    out.speedHalfGrip        = speed[0];
    out.speedLoadEquivalency = speed[1];
    out.camberPeakDeg  = camber[0];
    out.camberLatGain  = camber[1];
    out.camberLongLoss = camber[2];
    out.rollingResistance = r.Number(a, at, "rollingResistance");
    out.verticalStiffness = r.Number(a, at, "verticalStiffness");
    r.Positive(out.camberPeakDeg, at + ".camber[0]");
    r.Positive(out.verticalStiffness, at + ".verticalStiffness");
}

bool ReadTires(const fs::path& path, VehicleDesc& out, std::string* error)
{
    json root;
    if (!ReadJson(path, root, error))
        return false;
    StrictReader r(path.string());
    r.CheckObject(root, "", { "format", "version", "source", "curves", "front", "rear" });
    r.Header(root, "FoxTires");
    if (!r.Failed())
    {
        ReadTireAxle(r, root, "front", out.front.tire);
        ReadTireAxle(r, root, "rear", out.rear.tire);
    }
    return r.Failed() ? Fail(error, r.Error()) : true;
}

void ReadRake(StrictReader& r, const json& element, const std::string& at, const char* key,
              AeroParams::Rake& out)
{
    const json& rake = r.Object(element, at, key, { "noseDown", "noseUp" });
    const std::string rakeAt = StrictReader::Join(at, key);
    for (auto [name, term] : { std::pair<const char*, AeroParams::RakeTerm*>{ "noseDown", &out.noseDown },
                               std::pair<const char*, AeroParams::RakeTerm*>{ "noseUp", &out.noseUp } })
    {
        float v[3] = {};
        r.Numbers(rake, rakeAt, name, v);
        *term = { v[0], v[1], v[2] };
    }
}

void ReadYaw(StrictReader& r, const json& element, const std::string& at, AeroParams::Yaw& out)
{
    const json& yaw = r.Object(element, at, "yaw", { "sideways", "peakDeg", "peakMult" });
    const std::string yawAt = at + ".yaw";
    out.sideways = r.Number(yaw, yawAt, "sideways");
    out.peakDeg  = r.Number(yaw, yawAt, "peakDeg");
    out.peakMult = r.Number(yaw, yawAt, "peakMult");
}

void ReadDirectional(StrictReader& r, const json& element, const std::string& at,
                     AeroParams::Directional& out)
{
    const json& d = r.Object(element, at, "directional", { "left", "right", "up", "down", "back" });
    const std::string dirAt = at + ".directional";
    out.left  = r.Vec3(d, dirAt, "left");
    out.right = r.Vec3(d, dirAt, "right");
    out.up    = r.Vec3(d, dirAt, "up");
    out.down  = r.Vec3(d, dirAt, "down");
    out.back  = r.Vec3(d, dirAt, "back");
}

// Keys shared by the wings and the body.
void ReadElement(StrictReader& r, const json& e, const std::string& at, bool hasYaw, AeroParams::Element& out)
{
    out.center = r.Vec3(e, at, "center");
    out.drag   = r.Number(e, at, "drag");
    out.lift   = r.Number(e, at, "lift");
    ReadRake(r, e, at, "rakeDrag", out.rakeDrag);
    ReadRake(r, e, at, "rakeLift", out.rakeLift);
    if (hasYaw)
        ReadYaw(r, e, at, out.yaw);
    ReadDirectional(r, e, at, out.directional);
}

void ReadDiffuser(StrictReader& r, const json& root, AeroParams::Diffuser& out)
{
    const json& d = r.Object(root, "", "diffuser", { "center", "lift", "yaw", "rearHeight", "frontHeight",
                                                     "rake", "limits", "stall", "undertrayHalfWidth" });
    const std::string at = "diffuser";
    out.center = r.Vec3(d, at, "center");
    out.lift   = r.Number(d, at, "lift");
    ReadYaw(r, d, at, out.yaw);
    r.Numbers(d, at, "rearHeight", out.rearHeight);
    r.Numbers(d, at, "frontHeight", out.frontHeight);
    r.Numbers(d, at, "rake", out.rake);
    r.Numbers(d, at, "limits", out.limits);
    r.Numbers(d, at, "stall", out.stall);
    out.undertrayHalfWidth = r.Number(d, at, "undertrayHalfWidth");
}

bool ReadAero(const fs::path& path, VehicleDesc& out, std::string* error)
{
    json root;
    if (!ReadJson(path, root, error))
        return false;
    StrictReader r(path.string());
    r.CheckObject(root, "", { "format", "version", "source", "airDensity", "staticRideHeight", "frontAxleZ",
                              "rearAxleZ", "frontWing", "rearWing", "body", "diffuser" });
    r.Header(root, "FoxAero");
    if (r.Failed())
        return Fail(error, r.Error());

    AeroParams& a = out.aero;
    a.airDensity = r.Number(root, "", "airDensity");
    r.Numbers(root, "", "staticRideHeight", a.staticRideHeight);
    a.frontAxleZ = r.Number(root, "", "frontAxleZ");
    a.rearAxleZ  = r.Number(root, "", "rearAxleZ");
    r.Positive(a.airDensity, "airDensity");
    if (!(a.frontAxleZ > a.rearAxleZ))
        r.Fail("frontAxleZ", "must be ahead of rearAxleZ");

    const json& fw = r.Object(root, "", "frontWing", { "center", "drag", "lift", "rakeDrag", "rakeLift", "yaw",
                                                       "directional", "heightLut" });
    ReadElement(r, fw, "frontWing", true, a.frontWing);
    const json& lut = r.Object(fw, "frontWing", "heightLut", { "range", "values" });
    r.Numbers(lut, "frontWing.heightLut", "range", a.frontWing.heightRange);
    a.frontWing.heightLut = r.Numbers(lut, "frontWing.heightLut", "values", 2, true);
    if (!(a.frontWing.heightRange[1] > a.frontWing.heightRange[0]))
        r.Fail("frontWing.heightLut.range", "must be increasing");

    const json& rw = r.Object(root, "", "rearWing", { "center", "drag", "lift", "rakeDrag", "rakeLift", "yaw",
                                                      "directional" });
    ReadElement(r, rw, "rearWing", true, a.rearWing);

    const json& body = r.Object(root, "", "body", { "center", "drag", "lift", "rakeDrag", "rakeLift",
                                                    "directional", "dragHeightAvg", "dragHeightDiff",
                                                    "maxHeight" });
    ReadElement(r, body, "body", false, a.body);
    a.body.dragHeightAvg  = r.Number(body, "body", "dragHeightAvg");
    a.body.dragHeightDiff = r.Number(body, "body", "dragHeightDiff");
    a.body.maxHeight      = r.Number(body, "body", "maxHeight");

    ReadDiffuser(r, root, a.diffuser);
    return r.Failed() ? Fail(error, r.Error()) : true;
}

bool FileReference(const json& j, const char* key, const fs::path& dir, const std::string& path, fs::path& out,
                   std::string* error)
{
    if (!j.contains(key) || !j.at(key).is_string())
        return Fail(error, path + ": \"" + key + "\" must name a file relative to the definition");
    out = dir / j.at(key).get<std::string>();
    return true;
}

bool ReadLayout(const fs::path& dir, const json& j, VehicleDesc& out, std::string* error)
{
    // Geometry generated by the asset converter.
    json layout;
    if (!ReadJson(dir / j.at("layout").get<std::string>(), layout, error))
        return false;
    static const char* k_wheelKeys[VehicleDesc::WheelCount] = { "fl", "fr", "rl", "rr" };
    for (int w = 0; w < VehicleDesc::WheelCount; ++w)
    {
        const json& lw = layout.at("wheels").at(k_wheelKeys[w]);
        out.wheels[w].center = ReadVec3(lw, "center", {});
        out.wheels[w].radius = lw.at("radius").get<float>();
    }
    for (const auto& p : layout.at("hullPoints"))
        out.chassis.hullPoints.push_back(
            { p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() });
    return true;
}

void ReadChassis(const json& j, VehicleDesc& out)
{
    const json& chassis = j.at("chassis");
    out.chassis.mass         = chassis.at("mass").get<float>();
    out.chassis.centerOfMass = ReadVec3(chassis, "centerOfMass", {});
    out.chassis.inertia      = ReadVec3(chassis, "inertia", out.chassis.inertia);
}

bool ReadEngine(const json& j, VehicleDesc& out, const std::string& path, std::string* error)
{
    const json& engine = j.at("engine");
    for (const auto& point : engine.at("torqueCurve"))
        out.engine.torqueCurve.emplace_back(point.at(0).get<float>(), point.at(1).get<float>());
    if (out.engine.torqueCurve.size() < 2)
        return Fail(error, path + ": engine.torqueCurve needs at least two points");
    out.engine.idleRpm        = engine.value("idleRpm", out.engine.idleRpm);
    out.engine.limiterRpm     = engine.value("limiterRpm", out.engine.limiterRpm);
    out.engine.inertia        = engine.value("inertia", out.engine.inertia);
    out.engine.frictionTorque = engine.value("frictionTorque", out.engine.frictionTorque);
    out.engine.frictionPerRad = engine.value("frictionPerRad", out.engine.frictionPerRad);
    return true;
}

bool ReadGearbox(const json& j, VehicleDesc& out, const std::string& path, std::string* error)
{
    const json& gearbox = j.at("gearbox");
    out.gearbox.ratios = gearbox.at("ratios").get<std::vector<float>>();
    if (out.gearbox.ratios.empty())
        return Fail(error, path + ": gearbox.ratios is empty");
    out.gearbox.reverseRatio     = gearbox.value("reverseRatio", out.gearbox.reverseRatio);
    out.gearbox.finalDrive       = gearbox.value("finalDrive", out.gearbox.finalDrive);
    out.gearbox.efficiency       = gearbox.value("efficiency", out.gearbox.efficiency);
    out.gearbox.shiftTime        = gearbox.value("shiftTime", out.gearbox.shiftTime);
    out.gearbox.clutchMaxTorque  = gearbox.value("clutchMaxTorque", out.gearbox.clutchMaxTorque);
    out.gearbox.autoUpshiftRpm   = gearbox.value("autoUpshiftRpm", out.gearbox.autoUpshiftRpm);
    out.gearbox.autoDownshiftRpm = gearbox.value("autoDownshiftRpm", out.gearbox.autoDownshiftRpm);
    return true;
}

void ReadDifferential(const json& j, VehicleDesc& out)
{
    if (!j.contains("differential"))
        return;
    const json& d = j.at("differential");
    out.differential.preload   = d.value("preload", out.differential.preload);
    out.differential.powerLock = d.value("powerLock", out.differential.powerLock);
    out.differential.coastLock = d.value("coastLock", out.differential.coastLock);
}

} // namespace

bool LoadVehicleDesc(const std::string& path, VehicleDesc& out, std::string* error)
{
    json j;
    if (!ReadJson(path, j, error))
        return false;
    const fs::path dir = fs::path(path).parent_path();

    try
    {
        out = VehicleDesc{};
        out.name = j.value("name", fs::path(path).stem().string());
        if (!ReadLayout(dir, j, out, error))
            return false;
        ReadChassis(j, out);
        ReadAxle(j.at("front"), out.front);
        ReadAxle(j.at("rear"), out.rear);
        fs::path tires, aero;
        if (!FileReference(j, "tires", dir, path, tires, error) || !ReadTires(tires, out, error) ||
            !FileReference(j, "aero", dir, path, aero, error) || !ReadAero(aero, out, error))
            return false;
        if (!ReadEngine(j, out, path, error) || !ReadGearbox(j, out, path, error))
            return false;
        ReadDifferential(j, out);
    }
    catch (const json::exception& e)
    {
        return Fail(error, path + ": " + e.what());
    }
    if (!out.front.driven && !out.rear.driven)
        return Fail(error, path + ": no driven axle");
    return true;
}

} // namespace SE
