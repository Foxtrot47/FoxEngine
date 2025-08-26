#include "Skybox.h"

#include "FileUtils.h"
#include "Material.h"

Skybox::Skybox(Graphics& gfx, float size, std::wstring cubemapPath)
    : size(size) {
    // Define cube vertices (-1 to 1, scaled by size)

    std::vector<Skybox::Vertex> vertices;
    std::vector<unsigned short> indices;
    MakeUvSphere(size, 20, 20, vertices, indices);

    Material::MaterialInstanceData mData = {};
    mData.name = "M_Skybox";
    mData.vsPath = GetShaderPath(L"SkyboxVS.cso");
    mData.psPath = GetShaderPath(L"SkyboxPS.cso");
    mData.texturePaths = std::unordered_map<int, std::wstring> {
      { 0, cubemapPath }
    };
    mData.hasDepthState = true;
    auto material = std::make_unique<Material>(gfx, mData);

    AddBind(std::make_unique<VertexBuffer>(gfx, vertices));

    AddIndexBuffer(std::make_unique<IndexBuffer>(gfx, indices));

    AddBind(std::move(material));

    AddBind(std::make_unique<Topology>(gfx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST));

    AddBind(std::make_unique<TransformConstantBuffer>(gfx));

}

void Skybox::Update(float deltaTime)
{
}

void Skybox::MakeUvSphere(float radius, int segU, int segV,
    std::vector<Skybox::Vertex>& vertices,
    std::vector<unsigned short>& indices)
{
    vertices.clear();
    indices.clear();
    vertices.reserve((segU + 1) * (segV + 1));

    for (int v = 0; v <= segV; ++v) {
        float phi = float(v) / segV * DirectX::XM_PI; // 0..PI
        float y = std::cos(phi);
        float r = std::sin(phi);
        float vCoord = float(v) / segV; // UV: v from 0 to 1
        for (int u = 0; u <= segU; ++u) {
            float theta = float(u) / segU * DirectX::XM_2PI; // 0..2PI
            float x = r * std::cos(theta);
            float z = r * std::sin(theta);
            float uCoord = float(u) / segU; // UV: u from 0 to 1

            Skybox::Vertex vert{};
            vert.position = { x * radius, y * radius, z * radius };
            vert.uv = { uCoord, vCoord }; // UV coordinates
            vertices.push_back(vert);
        }
    }

    // Indices (reverse winding for inside rendering)
    for (int v = 0; v < segV; ++v) {
        for (int u = 0; u < segU; ++u) {
            unsigned short i0 = (unsigned short)(v * (segU + 1) + u);
            unsigned short i1 = (unsigned short)(i0 + 1);
            unsigned short i2 = (unsigned short)(i0 + (segU + 1));
            unsigned short i3 = (unsigned short)(i2 + 1);

            // Reverse winding order for inside rendering
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i2); indices.push_back(i3); indices.push_back(i1);
        }
    }
}