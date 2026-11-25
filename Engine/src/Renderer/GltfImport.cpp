#include "Renderer/GltfImport.h"
#include "Engine/Core/Logger.h"

#pragma warning(push, 0)
#pragma warning(disable : 4996)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#pragma warning(pop)
#include <mikktspace.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace SE {
namespace {

namespace fs = std::filesystem;

using Mat4 = std::array<float, 16>; // cgltf column-major

constexpr Mat4 k_identity = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

struct Vec3 { float x, y, z; };

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

// Matches aiVector3D::NormalizeSafe: zero vectors stay zero.
Vec3 NormalizeSafe(const Vec3& v)
{
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 0.0f ? Vec3 { v.x / length, v.y / length, v.z / length } : v;
}

const char* ResultName(cgltf_result result)
{
    switch (result)
    {
    case cgltf_result_success:         return "cgltf_result_success";
    case cgltf_result_data_too_short:  return "cgltf_result_data_too_short";
    case cgltf_result_unknown_format:  return "cgltf_result_unknown_format";
    case cgltf_result_invalid_json:    return "cgltf_result_invalid_json";
    case cgltf_result_invalid_gltf:    return "cgltf_result_invalid_gltf";
    case cgltf_result_invalid_options: return "cgltf_result_invalid_options";
    case cgltf_result_file_not_found:  return "cgltf_result_file_not_found";
    case cgltf_result_io_error:        return "cgltf_result_io_error";
    case cgltf_result_out_of_memory:   return "cgltf_result_out_of_memory";
    case cgltf_result_legacy_gltf:     return "cgltf_result_legacy_gltf";
    default:                           return "cgltf_result_unknown";
    }
}

struct DataDeleter { void operator()(cgltf_data* data) const { cgltf_free(data); } };

bool Fail(const char* path, const std::string& message, std::string* error)
{
    if (error) *error = message;
    SE_LOG_ERROR("Mesh::Load '%s': %s", path, message.c_str());
    return false;
}

std::string DecodeUri(const char* uri)
{
    std::string decoded = uri;
    decoded.resize(cgltf_decode_uri(decoded.data()));
    return decoded;
}

const cgltf_image* DdsImage(const cgltf_data* data, const cgltf_texture* texture)
{
    for (cgltf_size i = 0; i < texture->extensions_count; ++i)
    {
        const cgltf_extension& extension = texture->extensions[i];
        if (!extension.name || std::strcmp(extension.name, "MSFT_texture_dds") != 0 || !extension.data)
            continue;
        const nlohmann::json json = nlohmann::json::parse(extension.data, nullptr, false);
        if (json.is_object() && json.contains("source") && json["source"].is_number_unsigned())
        {
            const auto source = json["source"].get<size_t>();
            if (source < data->images_count)
                return &data->images[source];
        }
    }
    return nullptr;
}

// Image path relative to the glTF file, exactly as written (percent-decoded).
std::string TexturePath(const cgltf_data* data, const cgltf_texture_view& view, const char* path)
{
    if (!view.texture)
        return {};
    const cgltf_image* image = DdsImage(data, view.texture);
    if (!image)
        image = view.texture->image;
    if (!image)
        return {};
    if (image->buffer_view || !image->uri || std::strncmp(image->uri, "data:", 5) == 0)
    {
        SE_LOG_WARN("glTF '%s': embedded image skipped", path);
        return {};
    }
    return DecodeUri(image->uri);
}

// Same semantics as the Assimp path's ExtractSubMeshMaterial / ResolveRoughnessWorkflow /
// DetectAlphaMode for glTF sources.
SubMeshInfo ReadMaterial(const cgltf_data* data, const cgltf_material* material, const char* path)
{
    SubMeshInfo out;
    if (!material)
        return out;

    const cgltf_pbr_metallic_roughness& mr = material->pbr_metallic_roughness;
    const cgltf_pbr_specular_glossiness& sg = material->pbr_specular_glossiness;
    const bool specGloss = material->has_pbr_specular_glossiness != 0;

    out.albedoPath = TexturePath(data, mr.base_color_texture, path);
    if (out.albedoPath.empty() && specGloss)
        out.albedoPath = TexturePath(data, sg.diffuse_texture, path);
    out.normalPath = TexturePath(data, material->normal_texture, path);

    if (specGloss)
        out.glossinessFactor = sg.glossiness_factor;
    out.roughnessPath = TexturePath(data, mr.metallic_roughness_texture, path);
    if (!out.roughnessPath.empty())
        out.workflow = MaterialWorkflow::PackedMR;
    else if (specGloss)
    {
        out.roughnessPath = TexturePath(data, sg.specular_glossiness_texture, path);
        if (!out.roughnessPath.empty())
            out.workflow = MaterialWorkflow::SpecularGloss;
        else
            // Texture-less spec/gloss: approximate with the scalar MR path.
            out.roughnessFactor = 1.0f - out.glossinessFactor;
    }
    if (!specGloss)
        out.roughnessFactor = mr.roughness_factor;

    out.emissivePath = TexturePath(data, material->emissive_texture, path);

    if (material->alpha_mode == cgltf_alpha_mode_mask)
        out.alphaMode = AlphaMode::Cutout;
    else if (material->alpha_mode == cgltf_alpha_mode_blend)
        out.alphaMode = AlphaMode::Transparent;
    out.alphaCutoff = material->alpha_cutoff;
    return out;
}

bool IsTriangles(const cgltf_primitive& primitive)
{
    return primitive.type == cgltf_primitive_type_triangles ||
           primitive.type == cgltf_primitive_type_triangle_strip ||
           primitive.type == cgltf_primitive_type_triangle_fan;
}

const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type,
                                    cgltf_size components)
{
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
    {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.type == type && attribute.index == 0 && attribute.data &&
            cgltf_num_components(attribute.data->type) == components)
            return attribute.data;
    }
    return nullptr;
}

std::vector<float> Unpack(const cgltf_accessor* accessor)
{
    std::vector<float> values(cgltf_accessor_unpack_floats(accessor, nullptr, 0));
    if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
        values.clear();
    return values;
}

// Primitive geometry in glTF space, shared by every node instance of its mesh.
struct PrimitiveGeometry
{
    std::vector<Vec3> positions, normals, tangents, bitangents;
    std::vector<float> uvs; // 2 per vertex; empty when TEXCOORD_0 is absent
    std::vector<uint32_t> indices; // triangle list, glTF (counter-clockwise) winding
};

bool ReadIndices(const cgltf_primitive& primitive, size_t vertexCount, std::vector<uint32_t>& out)
{
    std::vector<uint32_t> raw;
    if (const cgltf_accessor* accessor = primitive.indices)
    {
        raw.resize(accessor->count);
        if (cgltf_accessor_unpack_indices(accessor, raw.data(), sizeof(uint32_t), raw.size()) != raw.size())
        {
            // Sparse index accessors: unpack_floats applies the sparse substitution.
            const std::vector<float> values = Unpack(accessor);
            if (values.size() != raw.size())
                return false;
            for (size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<uint32_t>(values[i]);
        }
    }
    else
    {
        raw.resize(vertexCount);
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = static_cast<uint32_t>(i);
    }

    out.clear();
    if (primitive.type == cgltf_primitive_type_triangles)
    {
        raw.resize(raw.size() / 3 * 3);
        out = std::move(raw);
    }
    else if (raw.size() >= 3)
    {
        out.reserve((raw.size() - 2) * 3);
        for (size_t i = 0; i + 2 < raw.size(); ++i)
        {
            if (primitive.type == cgltf_primitive_type_triangle_fan)
                out.insert(out.end(), { raw[0], raw[i + 1], raw[i + 2] });
            else if (i % 2 == 0)
                out.insert(out.end(), { raw[i], raw[i + 1], raw[i + 2] });
            else
                out.insert(out.end(), { raw[i + 1], raw[i], raw[i + 2] });
        }
    }
    for (const uint32_t index : out)
        if (index >= vertexCount)
            return false;
    return true;
}

// Area-weighted face normals accumulated over vertices with bit-identical positions.
void GenerateNormals(PrimitiveGeometry& geometry)
{
    struct Key
    {
        uint32_t x, y, z;
        bool operator==(const Key& other) const { return x == other.x && y == other.y && z == other.z; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& key) const
        {
            return (size_t(key.x) * 73856093u) ^ (size_t(key.y) * 19349663u) ^ (size_t(key.z) * 83492791u);
        }
    };

    const size_t vertexCount = geometry.positions.size();
    std::unordered_map<Key, uint32_t, KeyHash> groups;
    groups.reserve(vertexCount);
    std::vector<uint32_t> group(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        Key key;
        std::memcpy(&key, &geometry.positions[v], sizeof(key));
        group[v] = groups.emplace(key, static_cast<uint32_t>(groups.size())).first->second;
    }

    std::vector<Vec3> sums(groups.size(), Vec3 { 0, 0, 0 });
    for (size_t f = 0; f + 2 < geometry.indices.size(); f += 3)
    {
        const uint32_t a = geometry.indices[f], b = geometry.indices[f + 1], c = geometry.indices[f + 2];
        const Vec3 face = Cross(Sub(geometry.positions[b], geometry.positions[a]),
                                Sub(geometry.positions[c], geometry.positions[a]));
        for (const uint32_t v : { a, b, c })
        {
            Vec3& sum = sums[group[v]];
            sum.x += face.x; sum.y += face.y; sum.z += face.z;
        }
    }

    geometry.normals.resize(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        const Vec3 n = NormalizeSafe(sums[group[v]]);
        geometry.normals[v] = n.x == 0.0f && n.y == 0.0f && n.z == 0.0f ? Vec3 { 0, 1, 0 } : n;
    }
}

struct MikkContext
{
    PrimitiveGeometry* geometry;
    std::vector<Vec3> tangentSum;
    std::vector<float> signSum;
};

MikkContext& Mikk(const SMikkTSpaceContext* context)
{
    return *static_cast<MikkContext*>(context->m_pUserData);
}

uint32_t MikkIndex(const SMikkTSpaceContext* context, int face, int vert)
{
    return Mikk(context).geometry->indices[size_t(face) * 3 + size_t(vert)];
}

void GenerateTangents(PrimitiveGeometry& geometry)
{
    const size_t vertexCount = geometry.positions.size();
    MikkContext state { &geometry, std::vector<Vec3>(vertexCount, Vec3 { 0, 0, 0 }),
                        std::vector<float>(vertexCount, 0.0f) };

    SMikkTSpaceInterface callbacks {};
    callbacks.m_getNumFaces = [](const SMikkTSpaceContext* context) {
        return static_cast<int>(Mikk(context).geometry->indices.size() / 3);
    };
    callbacks.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, const int) { return 3; };
    callbacks.m_getPosition = [](const SMikkTSpaceContext* context, float out[], const int face, const int vert) {
        const Vec3& p = Mikk(context).geometry->positions[MikkIndex(context, face, vert)];
        out[0] = p.x; out[1] = p.y; out[2] = p.z;
    };
    callbacks.m_getNormal = [](const SMikkTSpaceContext* context, float out[], const int face, const int vert) {
        const Vec3& n = Mikk(context).geometry->normals[MikkIndex(context, face, vert)];
        out[0] = n.x; out[1] = n.y; out[2] = n.z;
    };
    callbacks.m_getTexCoord = [](const SMikkTSpaceContext* context, float out[], const int face, const int vert) {
        const float* uv = Mikk(context).geometry->uvs.data() + size_t(MikkIndex(context, face, vert)) * 2;
        out[0] = uv[0]; out[1] = uv[1];
    };
    callbacks.m_setTSpaceBasic = [](const SMikkTSpaceContext* context, const float tangent[], const float sign,
                                    const int face, const int vert) {
        const uint32_t v = MikkIndex(context, face, vert);
        Vec3& sum = Mikk(context).tangentSum[v];
        sum.x += tangent[0]; sum.y += tangent[1]; sum.z += tangent[2];
        Mikk(context).signSum[v] += sign;
    };

    SMikkTSpaceContext context {};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &state;
    genTangSpaceDefault(&context);

    geometry.tangents.resize(vertexCount);
    geometry.bitangents.resize(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        const Vec3 t = NormalizeSafe(state.tangentSum[v]);
        if (t.x == 0.0f && t.y == 0.0f && t.z == 0.0f)
        {
            geometry.tangents[v] = { 1, 0, 0 };
            geometry.bitangents[v] = { 0, 1, 0 };
            continue;
        }
        // Assimp convention: the bitangent points towards decreasing V (see ReadGeometry).
        const float sign = state.signSum[v] < 0.0f ? 1.0f : -1.0f;
        const Vec3 b = Cross(geometry.normals[v], t);
        geometry.tangents[v] = t;
        geometry.bitangents[v] = { b.x * sign, b.y * sign, b.z * sign };
    }
}

bool ReadGeometry(const cgltf_primitive& primitive, PrimitiveGeometry& geometry)
{
    const cgltf_accessor* position = FindAttribute(primitive, cgltf_attribute_type_position, 3);
    if (!position)
        return false;
    const std::vector<float> positions = Unpack(position);
    const size_t vertexCount = position->count;
    if (positions.size() != vertexCount * 3)
        return false;
    geometry.positions.resize(vertexCount);
    std::memcpy(geometry.positions.data(), positions.data(), positions.size() * sizeof(float));

    if (!ReadIndices(primitive, vertexCount, geometry.indices))
        return false;

    const auto readVec3 = [vertexCount](const cgltf_accessor* accessor, std::vector<Vec3>& out) {
        if (!accessor || accessor->count != vertexCount)
            return false;
        const std::vector<float> values = Unpack(accessor);
        if (values.size() != vertexCount * 3)
            return false;
        out.resize(vertexCount);
        std::memcpy(out.data(), values.data(), values.size() * sizeof(float));
        return true;
    };
    if (!readVec3(FindAttribute(primitive, cgltf_attribute_type_normal, 3), geometry.normals))
        GenerateNormals(geometry);

    if (const cgltf_accessor* uv = FindAttribute(primitive, cgltf_attribute_type_texcoord, 2))
    {
        if (uv->count == vertexCount)
            geometry.uvs = Unpack(uv);
        if (geometry.uvs.size() != vertexCount * 2)
            geometry.uvs.clear();
    }

    const cgltf_accessor* tangent = FindAttribute(primitive, cgltf_attribute_type_tangent, 4);
    std::vector<float> tangents;
    if (tangent && tangent->count == vertexCount)
        tangents = Unpack(tangent);
    // glTF bitangents are cross(N, T) * w, pointing towards increasing V. The Assimp
    // path (the shading reference) negates every bitangent in ConvertToLeftHanded on
    // top of the Z mirror, so they are stored negated here.
    if (tangents.size() == vertexCount * 4)
    {
        geometry.tangents.resize(vertexCount);
        geometry.bitangents.resize(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            const float* t = tangents.data() + v * 4;
            geometry.tangents[v] = { t[0], t[1], t[2] };
            const Vec3 b = Cross(geometry.normals[v], geometry.tangents[v]);
            geometry.bitangents[v] = { -b.x * t[3], -b.y * t[3], -b.z * t[3] };
        }
    }
    else if (!geometry.uvs.empty())
        GenerateTangents(geometry);
    else
    {
        geometry.tangents.assign(vertexCount, Vec3 { 1, 0, 0 });
        geometry.bitangents.assign(vertexCount, Vec3 { 0, 1, 0 });
    }
    return true;
}

// Bakes one node instance into the output streams in engine space: world transform
// (normals, tangents and bitangents by its inverse transpose), Z mirrored and winding
// reversed, as Assimp's ConvertToLeftHanded produces. UVs stay as authored: Assimp's
// glTF reader flips V on load and FlipUVs flips it back.
void AppendInstance(const PrimitiveGeometry& geometry, const Mat4& world, const SubMeshInfo& material,
                    MeshSourceData& output)
{
    const bool transform = world != k_identity;
    // Upper 3x3, row-major: a[row][col] = world[col * 4 + row].
    float a[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a[r][c] = world[c * 4 + r];
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                      a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    // Inverse transpose = cofactor matrix / det.
    float normalMat[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            const float cofactor = a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1];
            normalMat[r][c] = det != 0.0f ? cofactor / det : cofactor;
        }

    const auto point = [&](const Vec3& p) {
        return Vec3 { a[0][0] * p.x + a[0][1] * p.y + a[0][2] * p.z + world[12],
                      a[1][0] * p.x + a[1][1] * p.y + a[1][2] * p.z + world[13],
                      a[2][0] * p.x + a[2][1] * p.y + a[2][2] * p.z + world[14] };
    };
    const auto direction = [&](const Vec3& d) {
        return NormalizeSafe({ normalMat[0][0] * d.x + normalMat[0][1] * d.y + normalMat[0][2] * d.z,
                               normalMat[1][0] * d.x + normalMat[1][1] * d.y + normalMat[1][2] * d.z,
                               normalMat[2][0] * d.x + normalMat[2][1] * d.y + normalMat[2][2] * d.z });
    };

    MeshSubMeshSource record;
    record.firstVertex = static_cast<uint32_t>(output.vertices.size());
    record.vertexCount = static_cast<uint32_t>(geometry.positions.size());
    output.vertices.resize(output.vertices.size() + geometry.positions.size());
    MeshVertex* vertex = output.vertices.data() + record.firstVertex;
    const bool hasUV = !geometry.uvs.empty();
    for (size_t v = 0; v < geometry.positions.size(); ++v, ++vertex)
    {
        Vec3 p = geometry.positions[v], n = geometry.normals[v];
        Vec3 t = geometry.tangents[v], b = geometry.bitangents[v];
        if (transform)
        {
            p = point(p);
            n = direction(n);
            t = direction(t);
            b = direction(b);
        }
        vertex->x = p.x;   vertex->y = p.y;   vertex->z = -p.z;
        vertex->nx = n.x;  vertex->ny = n.y;  vertex->nz = -n.z;
        vertex->tx = t.x;  vertex->ty = t.y;  vertex->tz = -t.z;
        vertex->bx = b.x;  vertex->by = b.y;  vertex->bz = -b.z;
        vertex->u = hasUV ? geometry.uvs[v * 2] : 0.0f;
        vertex->v = hasUV ? geometry.uvs[v * 2 + 1] : 0.0f;
        output.bounds.Expand({ vertex->x, vertex->y, vertex->z });
    }

    // The Z mirror reverses winding; a mirroring node transform reverses it back.
    const bool flip = det >= 0.0f;
    record.firstIndex = static_cast<uint32_t>(output.indices.size());
    record.indexCount = static_cast<uint32_t>(geometry.indices.size());
    output.indices.resize(output.indices.size() + geometry.indices.size());
    uint32_t* index = output.indices.data() + record.firstIndex;
    for (size_t f = 0; f < geometry.indices.size(); f += 3, index += 3)
    {
        index[0] = geometry.indices[f + (flip ? 2 : 0)];
        index[1] = geometry.indices[f + 1];
        index[2] = geometry.indices[f + (flip ? 0 : 2)];
    }

    record.material = material;
    output.subMeshes.push_back(std::move(record));
}

// World transforms per mesh, depth-first in node order; a mesh referenced by several
// nodes gets one entry per instance.
void CollectInstances(const cgltf_data* data, const cgltf_node* node, std::vector<std::vector<Mat4>>& out)
{
    if (node->mesh)
    {
        Mat4 world;
        cgltf_node_transform_world(node, world.data());
        out[size_t(node->mesh - data->meshes)].push_back(world);
    }
    for (cgltf_size i = 0; i < node->children_count; ++i)
        CollectInstances(data, node->children[i], out);
}

} // namespace

bool ImportGltf(const char* path, MeshSourceData& output,
                const Mesh::ImportProgressCallback& progress, std::string* error)
{
    output = {};
    const auto cancelled = [&](float value) {
        if (!progress || progress(value))
            return false;
        if (error) *error = "cancelled";
        return true;
    };
    if (cancelled(0.0f))
        return false;

    cgltf_options options {};
    cgltf_data* parsed = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path, &parsed);
    const std::unique_ptr<cgltf_data, DataDeleter> data(parsed);
    if (result != cgltf_result_success)
        return Fail(path, std::string("glTF: ") + ResultName(result) + " in " + path, error);
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i)
    {
        const char* name = data->extensions_required[i];
        if (std::strcmp(name, "KHR_draco_mesh_compression") == 0 || std::strcmp(name, "EXT_meshopt_compression") == 0)
            return Fail(path, std::string("glTF: unsupported compression ") + name, error);
    }
    result = cgltf_load_buffers(&options, data.get(), path);
    if (result == cgltf_result_success)
        result = cgltf_validate(data.get());
    if (result != cgltf_result_success)
        return Fail(path, std::string("glTF: ") + ResultName(result) + " in " + path, error);
    if (cancelled(0.5f))
        return false;

    const fs::path directory = fs::path(path).parent_path();
    output.sourceFiles.push_back(fs::path(path).lexically_normal().generic_string());
    for (cgltf_size i = 0; i < data->buffers_count; ++i)
    {
        const char* uri = data->buffers[i].uri;
        if (uri && std::strncmp(uri, "data:", 5) != 0)
            output.sourceFiles.push_back((directory / DecodeUri(uri)).lexically_normal().generic_string());
    }
    std::sort(output.sourceFiles.begin(), output.sourceFiles.end());
    output.sourceFiles.erase(std::unique(output.sourceFiles.begin(), output.sourceFiles.end()),
                             output.sourceFiles.end());
    output.directory = Mesh::SourceDirectory(path);
    output.bounds = AABB {};

    std::vector<std::vector<Mat4>> instances(data->meshes_count);
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? data->scenes : nullptr);
    if (scene)
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            CollectInstances(data.get(), scene->nodes[i], instances);

    std::vector<SubMeshInfo> materials(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i)
        materials[i] = ReadMaterial(data.get(), &data->materials[i], path);
    const SubMeshInfo defaultMaterial;

    // Mesh-major order (all instances of a primitive together), as the Assimp path emits.
    uint64_t totalVertices = 0;
    uint64_t totalIndices = 0;
    size_t totalSubMeshes = 0;
    for (cgltf_size m = 0; m < data->meshes_count; ++m)
    {
        // Meshes no node references keep identity placement.
        if (instances[m].empty())
            instances[m].push_back(k_identity);
        const cgltf_mesh& mesh = data->meshes[m];
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p)
        {
            const cgltf_primitive& primitive = mesh.primitives[p];
            const cgltf_accessor* position = FindAttribute(primitive, cgltf_attribute_type_position, 3);
            if (!position || !IsTriangles(primitive))
                continue;
            const uint64_t count = primitive.indices ? primitive.indices->count : position->count;
            const uint64_t indexCount = primitive.type == cgltf_primitive_type_triangles
                ? count : (count >= 3 ? (count - 2) * 3 : 0);
            totalVertices += uint64_t(position->count) * instances[m].size();
            totalIndices += indexCount * instances[m].size();
            totalSubMeshes += instances[m].size();
        }
    }
    if (totalVertices > UINT32_MAX || totalIndices > UINT32_MAX)
        return Fail(path, "mesh exceeds 2^32 vertices or indices", error);
    output.vertices.reserve(static_cast<size_t>(totalVertices));
    output.indices.reserve(static_cast<size_t>(totalIndices));
    output.subMeshes.reserve(totalSubMeshes);

    size_t emitted = 0;
    const float total = static_cast<float>((std::max)(totalSubMeshes, size_t(1)));
    for (cgltf_size m = 0; m < data->meshes_count; ++m)
    {
        const cgltf_mesh& mesh = data->meshes[m];
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p)
        {
            const cgltf_primitive& primitive = mesh.primitives[p];
            if (!IsTriangles(primitive))
            {
                SE_LOG_WARN("glTF '%s': skipped non-triangle primitive", path);
                continue;
            }
            if (!FindAttribute(primitive, cgltf_attribute_type_position, 3))
            {
                SE_LOG_WARN("glTF '%s': skipped primitive without POSITION", path);
                continue;
            }
            PrimitiveGeometry geometry;
            if (!ReadGeometry(primitive, geometry))
                return Fail(path, std::string("glTF: unreadable primitive ") + std::to_string(p) +
                                  " of mesh " + std::to_string(m), error);
            if (geometry.indices.empty())
                continue;

            const SubMeshInfo& material = primitive.material
                ? materials[size_t(primitive.material - data->materials)] : defaultMaterial;
            for (const Mat4& world : instances[m])
            {
                if (cancelled(0.5f + 0.5f * static_cast<float>(emitted) / total))
                    return false;
                AppendInstance(geometry, world, material, output);
                ++emitted;
            }
        }
    }

    if (cancelled(1.0f))
        return false;
    SE_LOG_INFO("Mesh imported: '%s' — %zu sub-mesh(es)", path, output.subMeshes.size());
    return true;
}

} // namespace SE
