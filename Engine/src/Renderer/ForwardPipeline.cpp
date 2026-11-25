#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Core/Logger.h"
#include "Engine/Renderer/MeshVertexLayout.h"
#include "Engine/Renderer/PrimitiveMesh.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace SE {

namespace {

void BuildWireAABB(std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    // Unit cube corners (-1..1); scaled in draw call to match AABB half-extents.
    static const float c[8][3] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1},
    };
    for (auto& v : c) { MeshVertex mv = {}; mv.x = v[0]; mv.y = v[1]; mv.z = v[2]; verts.push_back(mv); }

    static const uint32_t edges[24] = {
        0,1, 1,2, 2,3, 3,0,   // front face
        4,5, 5,6, 6,7, 7,4,   // back face
        0,4, 1,5, 2,6, 3,7,   // connecting edges
    };
    for (auto i : edges) indices.push_back(i);
}

// Horizontal plane in XZ, Y=0, normal=(0,1,0). One quad tiled by uvScale.
void BuildPlaneMesh(float uvScale, std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    // 4 corners of a unit quad in XZ plane
    // Tangent = +X, Bitangent = +Z (standard horizontal plane TBN)
    MeshVertex v[4] = {};
    float uvs[4][2] = { {0,0}, {uvScale,0}, {uvScale,uvScale}, {0,uvScale} };
    float xs[4] = { -1,  1,  1, -1 };
    float zs[4] = { -1, -1,  1,  1 };
    for (int i = 0; i < 4; ++i)
    {
        v[i].x = xs[i]; v[i].y = 0.0f; v[i].z = zs[i];
        v[i].nx = 0.0f; v[i].ny = 1.0f; v[i].nz = 0.0f;
        v[i].u  = uvs[i][0]; v[i].v = uvs[i][1];
        v[i].u1 = v[i].u2 = v[i].u;
        v[i].v1 = v[i].v2 = v[i].v;
        v[i].tx = 1.0f; v[i].ty = 0.0f; v[i].tz = 0.0f;  // tangent = +X
        v[i].bx = 0.0f; v[i].by = 0.0f; v[i].bz = 1.0f;  // bitangent = +Z
        verts.push_back(v[i]);
    }
    // Two triangles (CCW from above)
    indices.push_back(0); indices.push_back(2); indices.push_back(1);
    indices.push_back(0); indices.push_back(3); indices.push_back(2);
}

void BuildWireSphere(int segs, std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    // Three great circles (XZ, XY, YZ planes); radius=1, scaled in draw call.
    for (int ring = 0; ring < 3; ++ring)
    {
        auto base = static_cast<uint32_t>(verts.size());
        for (int i = 0; i < segs; ++i)
        {
            float t = DirectX::XM_2PI * i / segs;
            float c = cosf(t), s = sinf(t);
            MeshVertex v = {};
            if      (ring == 0) { v.x = c; v.y = 0; v.z = s; }
            else if (ring == 1) { v.x = c; v.y = s; v.z = 0; }
            else                { v.x = 0; v.y = c; v.z = s; }
            verts.push_back(v);
        }
        for (int i = 0; i < segs; ++i)
        {
            indices.push_back(base + i);
            indices.push_back(base + (i + 1) % static_cast<uint32_t>(segs));
        }
    }
}

// MaterialBinding::textures order: shader register and whether the sRGB view binds.
struct MaterialTextureSlot { uint32_t reg; bool srgb; };
constexpr MaterialTextureSlot k_materialTextureSlots[] = {
    { 0, true }, { 1, false }, { 2, false }, { 7, false }, { 9, true }, { 13, false },
    { 14, true }, { 15, false },                                         // detail albedo, normal
    { 16, true }, { 17, true }, { 18, true }, { 19, true },              // terrain layer albedo A-D
    { 20, false }, { 21, true },                                         // terrain splat, overlay
    { 22, false }, { 23, false }, { 24, false }, { 25, false },          // terrain layer normal A-D
    { 26, false }, { 27, false },                                        // clearcoat, coat roughness
};

} // anonymous namespace

bool ForwardPipeline::Init(ID3D11Device* device, AssetManager& assets, ShaderLibrary& shaders)
{
    const ShaderPermutation* perm = shaders.Get(L"Engine/Shaders/Basic.hlsl");
    if (!perm)
    {
        SE_LOG_ERROR("ForwardPipeline: failed to compile Basic.hlsl via ShaderLibrary");
        return false;
    }
    m_vs = perm->vs;
    m_ps = perm->ps;
    const ShaderPermutation* terrainPerm = shaders.Get(L"Engine/Shaders/Basic.hlsl", { { "FOX_TERRAIN", "1" } });
    if (!terrainPerm)
    {
        SE_LOG_ERROR("ForwardPipeline: failed to compile Basic.hlsl (FOX_TERRAIN) via ShaderLibrary");
        return false;
    }
    m_terrainPerm = *terrainPerm;

    SE_HR(device->CreateInputLayout(k_meshInputLayout, static_cast<UINT>(std::size(k_meshInputLayout)),
        perm->vsBlob->GetBufferPointer(), perm->vsBlob->GetBufferSize(), &m_layout));

    if (!m_transformCB.Create(device)) return false;
    if (!m_materialCB.Create(device))  return false;
    if (!m_shadowCB.Create(device))    return false;
    if (!m_planarCB.Create(device))    return false;

    if (!m_sampler.Create(device, { FilterMode::Anisotropic, AddressMode::Wrap }))
        return false;

    // Linear-clamp sampler for point shadow cube map lookups (s2)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        SE_HR(device->CreateSamplerState(&sd, m_cubeSampler.GetAddressOf()));
    }

    {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        BuildSphereGeometry(1.0f, 16, 16, verts, idx);
        m_sphereVB.Create(device, verts.data(),
            static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)), sizeof(MeshVertex));
        m_sphereIB.Create(device, idx.data(), static_cast<uint32_t>(idx.size()));
    }
    {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        BuildWireSphere(32, verts, idx);
        m_wireSphereVB.Create(device, verts.data(),
            static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)), sizeof(MeshVertex));
        m_wireSphereIB.Create(device, idx.data(), static_cast<uint32_t>(idx.size()));
    }
    {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        BuildWireAABB(verts, idx);
        m_wireAABBVB.Create(device, verts.data(),
            static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)), sizeof(MeshVertex));
        m_wireAABBIB.Create(device, idx.data(), static_cast<uint32_t>(idx.size()));
    }
    {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        BuildPlaneMesh(4.0f, verts, idx);  // UV tiled 4x over the unit quad
        m_planeVB.Create(device, verts.data(),
            static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)), sizeof(MeshVertex));
        m_planeIB.Create(device, idx.data(), static_cast<uint32_t>(idx.size()));
    }

    {
        D3D11_BUFFER_DESC bd  = {};
        bd.Usage              = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth          = sizeof(MeshVertex) * 2;
        bd.BindFlags          = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;
        SE_HR(device->CreateBuffer(&bd, nullptr, &m_lineBuffer));
    }

    m_defaultWhite  = assets.GetDefaultWhite();
    m_defaultBlack  = assets.GetDefaultBlack();
    m_defaultNormal = assets.GetDefaultNormal();

    // Alpha blend state for transparent materials
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        SE_HR(device->CreateBlendState(&bd, m_alphaBlend.GetAddressOf()));
    }

    // Two-sided rasterizer for cutout/transparent foliage
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        SE_HR(device->CreateRasterizerState(&rd, m_noCullRS.GetAddressOf()));
        rd.FillMode = D3D11_FILL_WIREFRAME;
        SE_HR(device->CreateRasterizerState(&rd, m_wireframeRS.GetAddressOf()));
        // DrawWireMesh: edges pulled toward the camera so they win the depth
        // test against the shaded surface they lie on.
        rd.DepthBias = -8;
        rd.SlopeScaledDepthBias = -1.5f;
        SE_HR(device->CreateRasterizerState(&rd, m_wireOverlayRS.GetAddressOf()));
    }

    return true;
}

void ForwardPipeline::Begin(ID3D11DeviceContext* ctx, DirectX::XMMATRIX view, DirectX::XMMATRIX proj)
{
    m_view = view;
    m_proj = proj;
    // Zero-velocity defaults; SetTemporalMatrices() overrides for TAA frames.
    m_viewProjNoJitter = DirectX::XMMatrixMultiply(view, proj);
    m_prevViewProj     = m_viewProjNoJitter;
    m_frustum.ExtractFromVP(DirectX::XMMatrixMultiply(view, proj));
    m_queue.Clear();
    m_queuedDraws.clear();
    m_lastDrawCalls = 0;
    m_lastCulled = 0;
    m_lastInstancedDraws = 0;
    m_lastInstancedItems = 0;
    ApplyAlphaModeState(ctx, AlphaMode::Opaque);

    m_sampler.BindPS(ctx, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(m_layout.Get());
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);

    // Bind default black to t7 (metallic = 0 → dielectric). Draw calls with a metallic map override it.
    m_defaultBlack->BindPS(ctx, 7);
    // Bind default black to t9 (emissive = 0). Draw calls with an emissive map override it.
    m_defaultBlack->BindPSSrgb(ctx, 9);

    // Default: no point shadow casters; caller overrides with BindPointShadows().
    ForwardShadowCBData defaultShadow = {
        0, 0.0f, static_cast<float>(m_cutoutFrameIndex), m_cutoutDitherWidth };
    m_shadowCB.Update(ctx, defaultShadow);
    m_shadowCB.BindPS(ctx, 4);

    PlanarReflectionCBData planar = {};
    DirectX::XMStoreFloat4x4(&planar.reflectionViewProj, DirectX::XMMatrixIdentity());
    m_planarCB.Update(ctx, planar);
    m_planarCB.BindPS(ctx, 7);
}

void ForwardPipeline::BindEnvironment(ID3D11DeviceContext* ctx,
                                       ID3D11ShaderResourceView* irradianceSRV,
                                       ID3D11ShaderResourceView* specularSRV,
                                       ID3D11ShaderResourceView* brdfLutSRV)
{
    ctx->PSSetShaderResources(4, 1, &irradianceSRV);
    ctx->PSSetShaderResources(10, 1, &specularSRV);
    ctx->PSSetShaderResources(11, 1, &brdfLutSRV);
}

void ForwardPipeline::SetReflectionPass(ID3D11DeviceContext* ctx, bool enabled, float planeY)
{
    PlanarReflectionCBData data = {};
    DirectX::XMStoreFloat4x4(&data.reflectionViewProj, DirectX::XMMatrixIdentity());
    data.planeY = planeY;
    data.clipEnabled = enabled ? 1.0f : 0.0f;
    m_planarCB.Update(ctx, data);
    m_planarCB.BindPS(ctx, 7);
    ctx->RSSetState(enabled ? m_noCullRS.Get() : nullptr);
}

void ForwardPipeline::BindPlanarReflection(ID3D11DeviceContext* ctx,
                                            ID3D11ShaderResourceView* reflectionSRV,
                                            DirectX::XMMATRIX reflectionViewProj,
                                            float planeY, float strength)
{
    PlanarReflectionCBData data = {};
    DirectX::XMStoreFloat4x4(&data.reflectionViewProj, reflectionViewProj);
    data.planeY = planeY;
    data.planarStrength = strength;
    data.planarEnabled = reflectionSRV ? 1.0f : 0.0f;
    m_planarCB.Update(ctx, data);
    m_planarCB.BindPS(ctx, 7);
    ctx->PSSetShaderResources(12, 1, &reflectionSRV);
}

void ForwardPipeline::ClearPlanarReflection(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(12, 1, &nullSRV);
    PlanarReflectionCBData data = {};
    DirectX::XMStoreFloat4x4(&data.reflectionViewProj, DirectX::XMMatrixIdentity());
    m_planarCB.Update(ctx, data);
    m_planarCB.BindPS(ctx, 7);
}

void ForwardPipeline::BindPointShadows(ID3D11DeviceContext* ctx,
                                        ID3D11ShaderResourceView* psrv0,
                                        ID3D11ShaderResourceView* psrv1,
                                        int numCasters, float bias)
{
    ID3D11ShaderResourceView* srvs[2] = { psrv0, psrv1 };
    ctx->PSSetShaderResources(5, 2, srvs);
    ctx->PSSetSamplers(2, 1, m_cubeSampler.GetAddressOf());
    ForwardShadowCBData data = {
        numCasters, bias, static_cast<float>(m_cutoutFrameIndex), m_cutoutDitherWidth };
    m_shadowCB.Update(ctx, data);
    m_shadowCB.BindPS(ctx, 4);
}

void ForwardPipeline::SetCutoutDither(uint32_t frameIndex, float width)
{
    m_cutoutFrameIndex  = frameIndex;
    m_cutoutDitherWidth = width;
}

void ForwardPipeline::SetTemporalMatrices(DirectX::XMMATRIX prevViewProj,
                                          DirectX::XMMATRIX viewProjNoJitter)
{
    m_prevViewProj     = prevViewProj;
    m_viewProjNoJitter = viewProjNoJitter;
}

void ForwardPipeline::UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model)
{
    UploadTransformCB(ctx, model, model);
}

void ForwardPipeline::UploadTransformCB(ID3D11DeviceContext* ctx, DirectX::XMMATRIX model,
                                        DirectX::XMMATRIX prevModel)
{
    using namespace DirectX;
    TransformCBData cb = {};
    XMStoreFloat4x4(&cb.model,            model);
    XMStoreFloat4x4(&cb.view,             m_view);
    XMStoreFloat4x4(&cb.projection,       m_proj);
    XMStoreFloat4x4(&cb.prevModel,        prevModel);
    XMStoreFloat4x4(&cb.viewProjNoJitter, m_viewProjNoJitter);
    XMStoreFloat4x4(&cb.prevViewProj,     m_prevViewProj);
    m_transformCB.Update(ctx, cb);
    m_transformCB.BindVS(ctx, 0); m_transformCB.BindPS(ctx, 0);
}

void ForwardPipeline::UploadInstancedTransformCB(ID3D11DeviceContext* ctx, uint32_t instanceBase)
{
    using namespace DirectX;
    TransformCBData cb = {};
    XMStoreFloat4x4(&cb.view,             m_view);
    XMStoreFloat4x4(&cb.projection,       m_proj);
    XMStoreFloat4x4(&cb.viewProjNoJitter, m_viewProjNoJitter);
    XMStoreFloat4x4(&cb.prevViewProj,     m_prevViewProj);
    cb.instanceBase = instanceBase;
    cb.useInstances = 1;
    m_transformCB.Update(ctx, cb);
    m_transformCB.BindVS(ctx, 0); m_transformCB.BindPS(ctx, 0);
}

void ForwardPipeline::SubmitMesh(const Mesh& mesh, DirectX::XMMATRIX model,
                                  const std::vector<SubMat>& mats, bool transparent,
                                  const DirectX::XMMATRIX* prevModel)
{
    using namespace DirectX;

    // Frustum cull using the mesh's object-space AABB transformed to world space.
    if (!m_frustum.TestAABB(mesh.GetBounds(), model))
    {
        m_lastCulled += mesh.GetSubMeshCount();
        return;
    }

    // Fallback sort depth for submeshes without bounds: the mesh origin.
    const XMMATRIX modelView = XMMatrixMultiply(model, m_view);
    const float originDepth = XMVectorGetZ(modelView.r[3]);

    uint32_t drawIdx = static_cast<uint32_t>(m_queuedDraws.size());
    m_queuedDraws.push_back({ &mesh, &mats });

    const uint32_t count = mesh.GetSubMeshCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        float depth = originDepth;
        const AABB& bounds = mesh.GetSubMeshBounds(i);
        if (bounds.IsValid())
        {
            // A single submesh shares the mesh bounds tested above.
            if (count > 1 && !m_frustum.TestAABB(Frustum::TransformAABB(bounds, model)))
            {
                ++m_lastCulled;
                continue;
            }
            const XMFLOAT3 center = bounds.Center();
            depth = XMVectorGetZ(XMVector3Transform(XMLoadFloat3(&center), modelView));
        }

        RenderItem item;
        item.model        = model;
        item.prevModel    = prevModel ? *prevModel : model;
        item.meshIndex    = drawIdx;
        item.subMeshIndex = i;
        item.sortDepth    = depth;
        item.transparent  = transparent || (mats[i].alphaMode == AlphaMode::Transparent);
        m_queue.Push(item);
    }
}

void ForwardPipeline::ApplyAlphaModeState(ID3D11DeviceContext* ctx, AlphaMode mode)
{
    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    switch (mode)
    {
    case AlphaMode::Opaque:
        ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        ctx->RSSetState(nullptr);
        break;
    case AlphaMode::Cutout:
        ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        ctx->RSSetState(m_noCullRS.Get());
        break;
    case AlphaMode::Transparent:
        ctx->OMSetBlendState(m_alphaBlend.Get(), blendFactor, 0xFFFFFFFF);
        ctx->RSSetState(m_noCullRS.Get());
        break;
    }
    if (viewShading == ViewShading::Wireframe) ctx->RSSetState(m_wireframeRS.Get());
}

bool ForwardPipeline::MaterialBinding::operator==(const MaterialBinding& other) const
{
    return alphaMode == other.alphaMode && kind == other.kind &&
           std::memcmp(textures, other.textures, sizeof(textures)) == 0 &&
           std::memcmp(&cb, &other.cb, sizeof(cb)) == 0;
}

// Every draw must initialize MaterialCB. Leaving opaque draws untouched
// carries zero/stale parameters, which collapses roughness to 0.04 and can
// also leak alpha, emissive, metallic, or debug state from a previous draw.
void ForwardPipeline::BuildMaterialBinding(const SubMat& mat, MaterialBinding& out) const
{
    MaterialParamsCBData& mc = out.cb;
    mc = {};
    mc.albedoTint       = { m_globalTint.x * mat.tint.x, m_globalTint.y * mat.tint.y,
                            m_globalTint.z * mat.tint.z };
    mc.roughnessScale   = mat.roughnessScale * m_globalRoughnessScale;
    mc.metallic         = m_globalMetallic; // texture value scales by the global override
    mc.debugShadow      = m_globalDebugShadow;
    mc.unlit = viewShading == ViewShading::Lit ? 0.0f : static_cast<float>(viewShading) + 1.0f;
    mc.alphaCutoff      = mat.alphaMode == AlphaMode::Cutout ? mat.alphaCutoff : 0.0f;
    mc.emissiveIntensity = 1.0f;
    mc.emissiveColor    = mat.emissiveColor;
    mc.workflow         = static_cast<float>(mat.workflow);
    mc.glossinessFactor = mat.glossinessFactor;
    mc.receiveShadows   = mat.receiveShadows ? 1.0f : 0.0f;
    mc.occlusionStrength = mat.occlusionStrength;
    float uvSets[SlotCount];
    for (uint32_t s = 0; s < SlotCount; ++s)
    {
        const SlotTransform& slot = mat.slots[s];
        mc.slotST[s] = { slot.scale[0], slot.scale[1], slot.offset[0], slot.offset[1] };
        uvSets[s] = static_cast<float>(slot.uvSet);
    }
    mc.slotUVA   = { uvSets[0], uvSets[1], uvSets[2], uvSets[3] };
    mc.slotUVB   = { uvSets[4], uvSets[5], uvSets[6], mat.detailNormalStrength };
    mc.slotFlags = { mat.occlusion ? 1.0f : 0.0f, mat.detailAlbedo ? 1.0f : 0.0f,
                     mat.detailNormal ? 1.0f : 0.0f, static_cast<float>(mat.occlusionChannel) };
    const float detailNeutral = (mat.detailAlbedoNeutral > 0.0f && mat.detailAlbedoNeutral <= 1.0f)
                                    ? mat.detailAlbedoNeutral
                                    : k_defaultDetailAlbedoNeutral;
    mc.tintParams = { static_cast<float>(mat.vertexTint),
                      static_cast<float>(mat.detailAlbedoBlend),
                      mat.detailAlbedoStrength,
                      1.0f / detailNeutral };
    const TerrainMat& terrain = mat.terrain;
    const bool isTerrain = mat.kind == MaterialKind::Terrain;
    if (isTerrain)
    {
        mc.terrainScale = { terrain.layerScale[0], terrain.layerScale[1],
                            terrain.layerScale[2], terrain.layerScale[3] };
        mc.terrainParams = { static_cast<float>(terrain.layerCount),
                             terrain.albedoAlphaIsRoughness ? 1.0f : 0.0f,
                             terrain.overlay ? terrain.overlayStrength : 0.0f,
                             static_cast<float>(terrain.overlayUVSet) };
        mc.terrainParams2 = { static_cast<float>(terrain.splatUVSet), 0.0f, 0.0f, 0.0f };
        mc.terrainST = { terrain.splatUVScale[0], terrain.splatUVScale[1],
                         terrain.overlayUVScale[0], terrain.overlayUVScale[1] };
    }
    const uint32_t coatTexMask = (mat.clearcoatTex ? 1u : 0u) | (mat.clearcoatRoughnessTex ? 2u : 0u);
    const bool coatMaskFromAlpha = mat.alphaMode == AlphaMode::Opaque && mat.clearcoatMaskFromAlbedoAlpha;
    mc.clearcoat = { mat.clearcoat, mat.clearcoatRoughness,
                     coatMaskFromAlpha ? 1.0f : 0.0f, static_cast<float>(coatTexMask) };
    mc.clearcoatST[0] = { mat.clearcoatSlot.scale[0], mat.clearcoatSlot.scale[1],
                          mat.clearcoatSlot.offset[0], mat.clearcoatSlot.offset[1] };
    mc.clearcoatST[1] = { mat.clearcoatRoughnessSlot.scale[0], mat.clearcoatRoughnessSlot.scale[1],
                          mat.clearcoatRoughnessSlot.offset[0], mat.clearcoatRoughnessSlot.offset[1] };
    mc.clearcoatUV = { static_cast<float>(mat.clearcoatSlot.uvSet),
                       static_cast<float>(mat.clearcoatRoughnessSlot.uvSet), 0.0f, 0.0f };
    mc.billboardParams = { mat.billboard ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };

    // In k_materialTextureSlots order; null leaves the register unbound.
    const Texture2D** t = out.textures;
    t[0] = mat.albedo.get();
    t[1] = mat.roughness.get();
    t[2] = mat.normal.get();
    t[3] = (mat.metallic ? mat.metallic : m_defaultBlack).get();
    t[4] = (mat.emissive ? mat.emissive : m_defaultBlack).get();
    t[5] = (mat.occlusion ? mat.occlusion : m_defaultWhite).get();
    // Detail maps are sampled only when present (SlotFlags.y/z): a white default
    // would brighten the albedo by the mid-grey normalisation factor.
    t[6] = mat.detailAlbedo.get();
    t[7] = mat.detailNormal.get();
    for (uint32_t i = 0; i < k_maxTerrainLayers; ++i)
    {
        t[8 + i]  = isTerrain ? (terrain.layerAlbedo[i] ? terrain.layerAlbedo[i] : m_defaultWhite).get() : nullptr;
        t[14 + i] = isTerrain ? (terrain.layerNormal[i] ? terrain.layerNormal[i] : m_defaultNormal).get() : nullptr;
    }
    t[12] = isTerrain ? (terrain.splat ? terrain.splat : m_defaultBlack).get() : nullptr;
    t[13] = isTerrain ? (terrain.overlay ? terrain.overlay : m_defaultWhite).get() : nullptr;
    t[18] = mat.clearcoatTex.get();
    t[19] = mat.clearcoatRoughnessTex.get();
    out.alphaMode = mat.alphaMode;
    out.kind = mat.kind;
}

void ForwardPipeline::UploadMaterialCB(ID3D11DeviceContext* ctx, const MaterialParamsCBData& data)
{
    m_materialCB.Update(ctx, data);
    m_materialCB.BindVS(ctx, 3);
    m_materialCB.BindPS(ctx, 3);
}

void ForwardPipeline::BindMaterial(ID3D11DeviceContext* ctx, const MaterialBinding& binding)
{
    static_assert(std::size(k_materialTextureSlots) == k_materialTextureCount,
                  "k_materialTextureSlots must list every MaterialBinding texture");
    UploadMaterialCB(ctx, binding.cb);
    for (uint32_t i = 0; i < k_materialTextureCount; ++i)
    {
        const Texture2D* texture = binding.textures[i];
        if (!texture)
            continue;
        if (k_materialTextureSlots[i].srgb)
            texture->BindPSSrgb(ctx, k_materialTextureSlots[i].reg);
        else
            texture->BindPS(ctx, k_materialTextureSlots[i].reg);
    }
}

void ForwardPipeline::BindSubMaterial(ID3D11DeviceContext* ctx, const SubMat& mat)
{
    MaterialBinding binding;
    BuildMaterialBinding(mat, binding);
    BindMaterial(ctx, binding);
}

void ForwardPipeline::ApplyMaterialState(ID3D11DeviceContext* ctx, AlphaMode mode, MaterialKind kind,
                                         AlphaMode& boundMode, MaterialKind& boundKind)
{
    if (mode != boundMode)
    {
        ApplyAlphaModeState(ctx, mode);
        boundMode = mode;
    }
    if (kind != boundKind)
    {
        const bool terrain = kind == MaterialKind::Terrain;
        ctx->VSSetShader(terrain ? m_terrainPerm.vs.Get() : m_vs.Get(), nullptr, 0);
        ctx->PSSetShader(terrain ? m_terrainPerm.ps.Get() : m_ps.Get(), nullptr, 0);
        boundKind = kind;
    }
}

// Writes m_instanceData to the t29 structured buffer (grown by doubling) and binds it.
bool ForwardPipeline::UploadInstances(ID3D11DeviceContext* ctx)
{
    const uint32_t needed = static_cast<uint32_t>(m_instanceData.size());
    if (needed > m_instanceCapacity)
    {
        uint32_t capacity = (std::max)(m_instanceCapacity, 64u);
        while (capacity < needed)
            capacity *= 2;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        ctx->GetDevice(device.GetAddressOf());
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth           = capacity * static_cast<uint32_t>(sizeof(InstanceData));
        bd.Usage               = D3D11_USAGE_DYNAMIC;
        bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(InstanceData);
        m_instanceSRV.Reset();
        m_instanceBuffer.Reset();
        m_instanceCapacity = 0;
        if (FAILED(device->CreateBuffer(&bd, nullptr, m_instanceBuffer.GetAddressOf())))
        {
            SE_LOG_ERROR("ForwardPipeline: instance buffer (%u instances) creation failed", capacity);
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements  = capacity;
        SE_HR(device->CreateShaderResourceView(m_instanceBuffer.Get(), &sd, m_instanceSRV.GetAddressOf()));
        m_instanceCapacity = capacity;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, m_instanceData.data(), m_instanceData.size() * sizeof(InstanceData));
    ctx->Unmap(m_instanceBuffer.Get(), 0);
    ctx->VSSetShaderResources(29, 1, m_instanceSRV.GetAddressOf());
    return true;
}

void ForwardPipeline::DrawInstancedGroups(ID3D11DeviceContext* ctx, AlphaMode& boundMode,
                                          MaterialKind& boundKind)
{
    using namespace DirectX;
    const std::vector<RenderItem>& items = m_queue.Items();
    m_instanced.assign(items.size(), 0);

    // Candidates: opaque/cutout items, ordered so equal (mesh, submesh) form runs.
    m_candidates.clear();
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        if (items[i].transparent)
            continue;
        const QueuedDraw& draw = m_queuedDraws[items[i].meshIndex];
        m_candidates.push_back({ draw.mesh, &(*draw.mats)[items[i].subMeshIndex],
                                 items[i].subMeshIndex, i, 0 });
    }
    if (m_candidates.size() < k_minInstanceGroup)
        return;
    const auto sameSubMesh = [](const InstanceCandidate& a, const InstanceCandidate& b) {
        return a.mesh == b.mesh && a.subMesh == b.subMesh;
    };
    std::sort(m_candidates.begin(), m_candidates.end(),
        [](const InstanceCandidate& a, const InstanceCandidate& b) {
            if (a.mesh != b.mesh) return std::less<const Mesh*>()(a.mesh, b.mesh);
            if (a.subMesh != b.subMesh) return a.subMesh < b.subMesh;
            if (a.mat != b.mat) return std::less<const SubMat*>()(a.mat, b.mat);
            return a.item < b.item;
        });

    m_bindings.clear();
    m_groups.clear();
    m_instanceData.clear();
    for (size_t runBegin = 0; runBegin < m_candidates.size();)
    {
        size_t runEnd = runBegin + 1;
        while (runEnd < m_candidates.size() && sameSubMesh(m_candidates[runEnd], m_candidates[runBegin]))
            ++runEnd;
        if (runEnd - runBegin < k_minInstanceGroup)
        {
            runBegin = runEnd;
            continue;
        }

        // Bindings are compared only within this (mesh, submesh) run; candidates
        // sharing a SubMat share its binding without rebuilding it.
        const uint32_t runBindings = static_cast<uint32_t>(m_bindings.size());
        for (size_t c = runBegin; c < runEnd; ++c)
        {
            InstanceCandidate& candidate = m_candidates[c];
            if (c > runBegin && candidate.mat == m_candidates[c - 1].mat)
            {
                candidate.binding = m_candidates[c - 1].binding;
                continue;
            }
            m_bindings.emplace_back();
            BuildMaterialBinding(*candidate.mat, m_bindings.back());
            candidate.binding = static_cast<uint32_t>(m_bindings.size() - 1);
            for (uint32_t b = runBindings; b + 1 < m_bindings.size(); ++b)
                if (m_bindings[b] == m_bindings.back())
                {
                    candidate.binding = b;
                    m_bindings.pop_back();
                    break;
                }
        }
        std::sort(m_candidates.begin() + runBegin, m_candidates.begin() + runEnd,
            [](const InstanceCandidate& a, const InstanceCandidate& b) {
                return a.binding != b.binding ? a.binding < b.binding : a.item < b.item;
            });

        for (size_t groupBegin = runBegin; groupBegin < runEnd;)
        {
            size_t groupEnd = groupBegin + 1;
            while (groupEnd < runEnd && m_candidates[groupEnd].binding == m_candidates[groupBegin].binding)
                ++groupEnd;
            if (groupEnd - groupBegin >= k_minInstanceGroup)
            {
                InstanceGroup group;
                group.firstInstance = static_cast<uint32_t>(m_instanceData.size());
                group.count         = static_cast<uint32_t>(groupEnd - groupBegin);
                group.item          = m_candidates[groupBegin].item;
                group.binding       = m_candidates[groupBegin].binding;
                group.depth         = items[group.item].sortDepth;
                for (size_t c = groupBegin; c < groupEnd; ++c)
                {
                    const RenderItem& item = items[m_candidates[c].item];
                    InstanceData data;
                    XMStoreFloat4x4(&data.model, item.model);
                    XMStoreFloat4x4(&data.prevModel, item.prevModel);
                    m_instanceData.push_back(data);
                    m_instanced[m_candidates[c].item] = 1;
                    group.depth = (std::min)(group.depth, item.sortDepth);
                }
                m_groups.push_back(group);
            }
            groupBegin = groupEnd;
        }
        runBegin = runEnd;
    }
    if (m_groups.empty())
        return;
    if (!UploadInstances(ctx))
    {
        std::fill(m_instanced.begin(), m_instanced.end(), uint8_t(0)); // draw them individually
        return;
    }

    // Front-to-back by each group's nearest member, like the sorted opaque path.
    std::sort(m_groups.begin(), m_groups.end(),
        [](const InstanceGroup& a, const InstanceGroup& b) { return a.depth < b.depth; });
    for (const InstanceGroup& group : m_groups)
    {
        const MaterialBinding& binding = m_bindings[group.binding];
        ApplyMaterialState(ctx, binding.alphaMode, binding.kind, boundMode, boundKind);
        BindMaterial(ctx, binding);
        UploadInstancedTransformCB(ctx, group.firstInstance);
        const RenderItem& item = items[group.item];
        m_queuedDraws[item.meshIndex].mesh->DrawSubMeshInstanced(ctx, item.subMeshIndex, group.count);
        ++m_lastDrawCalls;
        ++m_lastInstancedDraws;
        m_lastInstancedItems += group.count;
    }
}

void ForwardPipeline::Flush(ID3D11DeviceContext* ctx)
{
    m_queue.Sort();

    AlphaMode boundMode = AlphaMode::Opaque;
    MaterialKind boundKind = MaterialKind::Standard; // Begin() binds the standard permutation

    DrawInstancedGroups(ctx, boundMode, boundKind);

    const std::vector<RenderItem>& items = m_queue.Items();
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i < m_instanced.size() && m_instanced[i])
            continue;
        const RenderItem& item = items[i];
        const QueuedDraw& draw = m_queuedDraws[item.meshIndex];
        const SubMat& mat = (*draw.mats)[item.subMeshIndex];

        UploadTransformCB(ctx, item.model, item.prevModel);
        ApplyMaterialState(ctx, mat.alphaMode, mat.kind, boundMode, boundKind);
        BindSubMaterial(ctx, mat);
        draw.mesh->DrawSubMesh(ctx, item.subMeshIndex);
        ++m_lastDrawCalls;
    }

    if (boundMode != AlphaMode::Opaque)
        ApplyAlphaModeState(ctx, AlphaMode::Opaque);
    if (boundKind != MaterialKind::Standard)
    {
        ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    }
}

void ForwardPipeline::SetMaterialParams(ID3D11DeviceContext* ctx,
                                         DirectX::XMFLOAT3 tint, float roughnessScale, float metallic,
                                         float debugShadow, float emissiveIntensity,
                                         DirectX::XMFLOAT3 emissiveColor)
{
    MaterialParamsCBData mc = {};
    mc.albedoTint     = tint;
    mc.roughnessScale = roughnessScale;
    mc.metallic       = metallic;
    mc.debugShadow    = debugShadow;
    mc.unlit = viewShading == ViewShading::Lit ? 0.0f : static_cast<float>(viewShading) + 1.0f;
    mc.emissiveIntensity = emissiveIntensity;
    mc.emissiveColor  = emissiveColor;
    mc.receiveShadows = 1.0f; // slot transforms, occlusion, detail, vertex tint, terrain, clearcoat and billboard keep their neutral defaults
    UploadMaterialCB(ctx, mc);
}

void ForwardPipeline::SetGlobalMaterialParams(DirectX::XMFLOAT3 tint, float roughnessScale,
                                              float metallic, float debugShadow)
{
    m_globalTint = tint;
    m_globalRoughnessScale = roughnessScale;
    m_globalMetallic = metallic;
    m_globalDebugShadow = debugShadow;
}

void ForwardPipeline::DrawSphere(ID3D11DeviceContext* ctx,
                                  DirectX::XMFLOAT3 position, float radius,
                                  DirectX::XMFLOAT3 tint,
                                  const DirectX::XMMATRIX* prevModel)
{
    using namespace DirectX;

    SetMaterialParams(ctx, tint, 0.7f, 0.0f);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);
    m_defaultBlack->BindPS(ctx, 7);
    m_defaultBlack->BindPSSrgb(ctx, 9);

    const XMMATRIX model =
        XMMatrixScaling(radius, radius, radius) * XMMatrixTranslation(position.x, position.y, position.z);
    UploadTransformCB(ctx, model, prevModel ? *prevModel : model);

    m_sphereVB.Bind(ctx);
    m_sphereIB.Bind(ctx);
    ctx->DrawIndexed(m_sphereIB.GetCount(), 0, 0);
}

void ForwardPipeline::DrawWireSphere(ID3D11DeviceContext* ctx,
                                      DirectX::XMFLOAT3 position, float radius,
                                      DirectX::XMFLOAT3 color)
{
    using namespace DirectX;

    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);
    m_defaultBlack->BindPS(ctx, 7);
    m_defaultBlack->BindPSSrgb(ctx, 9);

    UploadTransformCB(ctx, XMMatrixScaling(radius, radius, radius) * XMMatrixTranslation(position.x, position.y, position.z));

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    m_wireSphereVB.Bind(ctx);
    m_wireSphereIB.Bind(ctx);
    ctx->DrawIndexed(m_wireSphereIB.GetCount(), 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ForwardPipeline::DrawWireAABB(ID3D11DeviceContext* ctx,
                                    DirectX::XMFLOAT3 mn, DirectX::XMFLOAT3 mx,
                                    DirectX::XMFLOAT3 color)
{
    using namespace DirectX;

    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);

    // Unit cube spans -1..1; scale by half-extents then translate to center.
    float hx = (mx.x - mn.x) * 0.5f, cx = (mn.x + mx.x) * 0.5f;
    float hy = (mx.y - mn.y) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
    float hz = (mx.z - mn.z) * 0.5f, cz = (mn.z + mx.z) * 0.5f;

    UploadTransformCB(ctx, XMMatrixScaling(hx, hy, hz) * XMMatrixTranslation(cx, cy, cz));

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    m_wireAABBVB.Bind(ctx);
    m_wireAABBIB.Bind(ctx);
    ctx->DrawIndexed(m_wireAABBIB.GetCount(), 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ForwardPipeline::DrawWireBox(ID3D11DeviceContext* ctx,
                                   DirectX::XMMATRIX world, DirectX::XMFLOAT3 color)
{
    using namespace DirectX;

    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);

    UploadTransformCB(ctx, world);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    m_wireAABBVB.Bind(ctx);
    m_wireAABBIB.Bind(ctx);
    ctx->DrawIndexed(m_wireAABBIB.GetCount(), 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ForwardPipeline::DrawWireMesh(ID3D11DeviceContext* ctx, const Mesh& mesh,
                                   DirectX::XMMATRIX world, DirectX::XMFLOAT3 color)
{
    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);

    UploadTransformCB(ctx, world);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> previous;
    ctx->RSGetState(previous.GetAddressOf());
    ctx->RSSetState(m_wireOverlayRS.Get());
    for (uint32_t i = 0; i < mesh.GetSubMeshCount(); ++i)
        if (mesh.GetSubMeshAlphaMode(i) == AlphaMode::Opaque)
            mesh.DrawSubMesh(ctx, i);
    ctx->RSSetState(previous.Get());
}

void ForwardPipeline::DrawLine(ID3D11DeviceContext* ctx,
                               DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 to,
                               DirectX::XMFLOAT3 color)
{
    using namespace DirectX;

    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);

    // Identity model — vertices are supplied in world space.
    UploadTransformCB(ctx, XMMatrixIdentity());

    MeshVertex verts[2] = {};
    verts[0].x = from.x; verts[0].y = from.y; verts[0].z = from.z;
    verts[1].x = to.x;   verts[1].y = to.y;   verts[1].z = to.z;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    SE_HR(ctx->Map(m_lineBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    memcpy(mapped.pData, verts, sizeof(verts));
    ctx->Unmap(m_lineBuffer.Get(), 0);

    UINT stride = sizeof(MeshVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_lineBuffer.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->Draw(2, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ForwardPipeline::DrawWireDisc(ID3D11DeviceContext* ctx,
                                    DirectX::XMFLOAT3 center, float radius,
                                    DirectX::XMFLOAT3 color)
{
    using namespace DirectX;

    MaterialParamsCBData mc = {};
    mc.albedoTint = color; mc.roughnessScale = 1.0f; mc.unlit = 1.0f;
    UploadMaterialCB(ctx, mc);

    m_defaultWhite->BindPSSrgb(ctx, 0);
    m_defaultWhite->BindPS(ctx, 1);
    m_defaultNormal->BindPS(ctx, 2);

    UploadTransformCB(ctx, XMMatrixScaling(radius, radius, radius) * XMMatrixTranslation(center.x, center.y, center.z));

    // Reuse ring 0 of the wire sphere mesh — it sits in the XZ plane (y=0).
    // 32 segments × 2 indices per line = 64 indices.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    m_wireSphereVB.Bind(ctx);
    m_wireSphereIB.Bind(ctx);
    ctx->DrawIndexed(64, 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ForwardPipeline::DrawPBRSphere(ID3D11DeviceContext* ctx,
                                     DirectX::XMFLOAT3 position, float radius,
                                     const SubMat& mat, float metallic, float roughnessScale,
                                     DirectX::XMFLOAT3 tint, float emissiveIntensity,
                                     DirectX::XMFLOAT3 emissiveColor,
                                     const DirectX::XMMATRIX* prevModel)
{
    using namespace DirectX;

    SetMaterialParams(ctx, tint, roughnessScale, metallic, 0.0f, emissiveIntensity, emissiveColor);

    mat.albedo    ? mat.albedo->BindPSSrgb(ctx, 0) : m_defaultWhite->BindPSSrgb(ctx, 0);
    mat.roughness ? mat.roughness->BindPS(ctx, 1) : m_defaultWhite->BindPS(ctx, 1);
    mat.normal    ? mat.normal->BindPS(ctx, 2)    : m_defaultNormal->BindPS(ctx, 2);
    mat.metallic  ? mat.metallic->BindPS(ctx, 7)  : m_defaultBlack->BindPS(ctx, 7);
    mat.emissive  ? mat.emissive->BindPSSrgb(ctx, 9)  : m_defaultBlack->BindPSSrgb(ctx, 9);

    const XMMATRIX model =
        XMMatrixScaling(radius, radius, radius) * XMMatrixTranslation(position.x, position.y, position.z);
    UploadTransformCB(ctx, model, prevModel ? *prevModel : model);

    m_sphereVB.Bind(ctx);
    m_sphereIB.Bind(ctx);
    ctx->DrawIndexed(m_sphereIB.GetCount(), 0, 0);
}

void ForwardPipeline::DrawPBRPlane(ID3D11DeviceContext* ctx,
                                    DirectX::XMFLOAT3 center, float halfSizeX, float halfSizeZ,
                                    const SubMat& mat, float metallic, float roughnessScale,
                                    DirectX::XMFLOAT3 tint, float emissiveIntensity,
                                    DirectX::XMFLOAT3 emissiveColor,
                                    const DirectX::XMMATRIX* prevModel)
{
    using namespace DirectX;

    SetMaterialParams(ctx, tint, roughnessScale, metallic, 0.0f, emissiveIntensity, emissiveColor);

    mat.albedo    ? mat.albedo->BindPSSrgb(ctx, 0) : m_defaultWhite->BindPSSrgb(ctx, 0);
    mat.roughness ? mat.roughness->BindPS(ctx, 1) : m_defaultWhite->BindPS(ctx, 1);
    mat.normal    ? mat.normal->BindPS(ctx, 2)    : m_defaultNormal->BindPS(ctx, 2);
    mat.metallic  ? mat.metallic->BindPS(ctx, 7)  : m_defaultBlack->BindPS(ctx, 7);
    mat.emissive  ? mat.emissive->BindPSSrgb(ctx, 9)  : m_defaultBlack->BindPSSrgb(ctx, 9);

    const XMMATRIX model =
        XMMatrixScaling(halfSizeX, 1.0f, halfSizeZ) * XMMatrixTranslation(center.x, center.y, center.z);
    UploadTransformCB(ctx, model, prevModel ? *prevModel : model);

    m_planeVB.Bind(ctx);
    m_planeIB.Bind(ctx);
    ctx->DrawIndexed(m_planeIB.GetCount(), 0, 0);
}

} // namespace SE
