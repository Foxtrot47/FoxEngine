#include <cmath>
#include <cstdio>
#include "Scripting/ScriptApi.h"
#include "Scripting/ScriptContext.h"
#include "Scripting/ScriptTypes.h"

namespace SE::Scripting {

namespace {

ScriptVec3 Add(const ScriptVec3& a, const ScriptVec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
ScriptVec3 Sub(const ScriptVec3& a, const ScriptVec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
ScriptVec3 Scale(const ScriptVec3& v, double s) { return { v.x * s, v.y * s, v.z * s }; }
double Dot(const ScriptVec3& a, const ScriptVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Length(const ScriptVec3& v) { return std::sqrt(Dot(v, v)); }

} // namespace

void BindMath(ScriptApi& api, ScriptContext& /*ctx*/)
{
    api.Global("vec3", "(x?: number, y?: number, z?: number) -> fox.Vec3",
        "Creates a fox.Vec3; missing components are 0.",
        [](sol::optional<double> x, sol::optional<double> y, sol::optional<double> z) {
            return ScriptVec3{ x.value_or(0.0), y.value_or(0.0), z.value_or(0.0) };
        });

    api.Class<ScriptVec3>("Vec3",
        "3D vector value. Operators return new vectors. Engine properties such as\n"
        "transform.position return copies: modify the copy, then assign it back\n"
        "(`local p = t.position; p.y = 2; t.position = p`).",
        sol::no_constructor)
        .Field("x", "number", "", &ScriptVec3::x)
        .Field("y", "number", "", &ScriptVec3::y)
        .Field("z", "number", "", &ScriptVec3::z)
        .Method("length", "() -> number", "Euclidean length.", &Length)
        .Method("normalized", "() -> fox.Vec3", "Unit-length copy; the zero vector stays zero.",
            [](const ScriptVec3& v) {
                const double len = Length(v);
                return len > 1e-12 ? Scale(v, 1.0 / len) : ScriptVec3{};
            })
        .Method("dot", "(other: fox.Vec3) -> number", "Dot product.", &Dot)
        .Method("cross", "(other: fox.Vec3) -> fox.Vec3", "Cross product (left-handed engine axes).",
            [](const ScriptVec3& a, const ScriptVec3& b) {
                return ScriptVec3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
            })
        .Method("lerp", "(other: fox.Vec3, t: number) -> fox.Vec3", "Linear interpolation; t = 0 gives self.",
            [](const ScriptVec3& a, const ScriptVec3& b, double t) { return Add(a, Scale(Sub(b, a), t)); })
        .Method("distance", "(other: fox.Vec3) -> number", "Distance between two points.",
            [](const ScriptVec3& a, const ScriptVec3& b) { return Length(Sub(a, b)); })
        .Static("is", "(value: any) -> boolean", "True if value is a fox.Vec3.",
            [](sol::object value) { return value.is<ScriptVec3>(); })
        .Operator(sol::meta_function::addition, "add(fox.Vec3): fox.Vec3", &Add)
        .Operator(sol::meta_function::subtraction, "sub(fox.Vec3): fox.Vec3", &Sub)
        .Operator(sol::meta_function::multiplication, "mul(number): fox.Vec3",
            sol::overload(
                [](const ScriptVec3& v, double s) { return Scale(v, s); },
                [](double s, const ScriptVec3& v) { return Scale(v, s); },
                [](const ScriptVec3& a, const ScriptVec3& b) { return ScriptVec3{ a.x * b.x, a.y * b.y, a.z * b.z }; }))
        .DocOperator("mul(fox.Vec3): fox.Vec3")
        .Operator(sol::meta_function::division, "div(number): fox.Vec3",
            [](const ScriptVec3& v, double s) { return Scale(v, 1.0 / s); })
        .Operator(sol::meta_function::unary_minus, "unm: fox.Vec3",
            [](const ScriptVec3& v) { return ScriptVec3{ -v.x, -v.y, -v.z }; })
        .Operator(sol::meta_function::equal_to, "",
            [](const ScriptVec3& a, const ScriptVec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; })
        .Operator(sol::meta_function::to_string, "",
            [](const ScriptVec3& v) {
                char text[96];
                snprintf(text, sizeof(text), "vec3(%g, %g, %g)", v.x, v.y, v.z);
                return std::string(text);
            });
}

} // namespace SE::Scripting
