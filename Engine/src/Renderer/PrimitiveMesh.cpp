#include "Engine/Renderer/PrimitiveMesh.h"

#include <cmath>

namespace SE {

using namespace DirectX;

void BuildSphereGeometry(float radius, int rings, int segments,
                         std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    for (int r = 0; r <= rings; ++r)
    {
        const float phi = -XM_PI * 0.5f + XM_PI * r / rings;
        for (int s = 0; s <= segments; ++s)
        {
            const float theta = XM_2PI * s / segments;
            const float cp = cosf(phi), sp = sinf(phi);
            const float ct = cosf(theta), st = sinf(theta);

            MeshVertex v;
            v.x  = cp * ct * radius; v.y = sp * radius; v.z = cp * st * radius;
            v.nx = cp * ct;          v.ny = sp;          v.nz = cp * st;
            v.u  = static_cast<float>(s) / segments;
            v.v  = static_cast<float>(r) / rings;
            v.u1 = v.u2 = v.u;
            v.v1 = v.v2 = v.v;
            v.tx = -st;      v.ty = 0.0f; v.tz = ct;
            v.bx = -sp * ct; v.by = cp;   v.bz = -sp * st;
            vertices.push_back(v);
        }
    }
    for (int r = 0; r < rings; ++r)
    {
        for (int s = 0; s < segments; ++s)
        {
            const uint32_t i0 = static_cast<uint32_t>(r * (segments + 1) + s);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(segments + 1);
            const uint32_t i3 = i2 + 1;
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }
}

void BuildBoxGeometry(XMFLOAT3 size, std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
{
    struct Face { XMFLOAT3 normal, tangent; };
    static const Face faces[6] = {
        { {  1, 0, 0 }, {  0, 0,  1 } }, { { -1, 0, 0 }, { 0, 0, -1 } },
        { {  0, 1, 0 }, {  1, 0,  0 } }, { {  0, -1, 0 }, { 1, 0, 0 } },
        { {  0, 0, 1 }, { -1, 0,  0 } }, { {  0, 0, -1 }, { 1, 0, 0 } },
    };
    const XMVECTOR half = XMVectorScale(XMLoadFloat3(&size), 0.5f);
    for (const Face& face : faces)
    {
        const XMVECTOR n = XMLoadFloat3(&face.normal);
        const XMVECTOR t = XMLoadFloat3(&face.tangent);
        const XMVECTOR b = XMVector3Cross(n, t);
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int corner = 0; corner < 4; ++corner)
        {
            const float su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            const float sv = corner >= 2 ? 1.0f : -1.0f;
            const XMVECTOR p = XMVectorMultiply(n + t * su + b * sv, half);
            MeshVertex v;
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&v.x), p);
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&v.nx), n);
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&v.tx), t);
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&v.bx), b);
            v.u = (su + 1.0f) * 0.5f;
            v.v = (sv + 1.0f) * 0.5f;
            v.u1 = v.u2 = v.u;
            v.v1 = v.v2 = v.v;
            vertices.push_back(v);
        }
        // Corners 0..3 run (-t,-b), (+t,-b), (+t,+b), (-t,+b). With b = n x t,
        // cross(p1 - p0, p2 - p0) = 4 t x (n x t) = 4n: clockwise seen from outside.
        for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u })
            indices.push_back(base + i);
    }
}

namespace {

MeshSourceData SingleSubMesh(std::vector<MeshVertex>&& vertices, std::vector<uint32_t>&& indices,
                             XMFLOAT3 extents, float roughness)
{
    MeshSourceData source;
    MeshSubMeshSource subMesh;
    subMesh.vertexCount = static_cast<uint32_t>(vertices.size());
    subMesh.indexCount = static_cast<uint32_t>(indices.size());
    subMesh.material.roughnessFactor = roughness;
    source.subMeshes.push_back(subMesh);
    source.vertices = std::move(vertices);
    source.indices = std::move(indices);
    source.bounds = AABB::FromCenterExtents({ 0.0f, 0.0f, 0.0f }, extents);
    return source;
}

} // namespace

MeshSourceData BuildSphereMeshSource(float radius, float roughness)
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    BuildSphereGeometry(radius, 16, 24, vertices, indices);
    return SingleSubMesh(std::move(vertices), std::move(indices), { radius, radius, radius }, roughness);
}

MeshSourceData BuildBoxMeshSource(XMFLOAT3 size, float roughness)
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    BuildBoxGeometry(size, vertices, indices);
    return SingleSubMesh(std::move(vertices), std::move(indices),
                         { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f }, roughness);
}

} // namespace SE
