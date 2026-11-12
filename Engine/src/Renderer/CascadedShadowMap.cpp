#include "Engine/Renderer/CascadedShadowMap.h"
#include "Engine/Core/Logger.h"
#include <cfloat>
#include <cmath>
#include <vector>
#include <algorithm>

namespace SE {

using namespace DirectX;

// Same sphere builder as ShadowMap (matching MeshVertex layout)
static void BuildCSMSphere(float radius, int rings, int segs,
    std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    const float pi = XM_PI;
    for (int r = 0; r <= rings; ++r)
    {
        float phi = -pi * 0.5f + pi * r / rings;
        for (int s = 0; s <= segs; ++s)
        {
            float theta = XM_2PI * s / segs;
            float cp = cosf(phi), sp = sinf(phi);
            float ct = cosf(theta), st = sinf(theta);
            MeshVertex v = {};
            v.x = cp * ct * radius; v.y = sp * radius; v.z = cp * st * radius;
            v.nx = cp * ct;         v.ny = sp;          v.nz = cp * st;
            verts.push_back(v);
        }
    }
    for (int r = 0; r < rings; ++r)
    {
        for (int s = 0; s < segs; ++s)
        {
            uint32_t i0 = static_cast<uint32_t>(r * (segs + 1) + s);
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + static_cast<uint32_t>(segs + 1);
            uint32_t i3 = i2 + 1;
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }
}

bool CascadedShadowMap::Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t resolution)
{
    m_resolution = resolution;

    m_perm = shaders.Get(L"Engine/Shaders/ShadowDepth.hlsl");
    if (!m_perm) { SE_LOG_ERROR("CSM: failed to compile ShadowDepth.hlsl"); return false; }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    SE_HR(device->CreateInputLayout(layoutDesc, 5,
        m_perm->vsBlob->GetBufferPointer(), m_perm->vsBlob->GetBufferSize(), &m_layout));

    // Texture2DArray: one slice per cascade
    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = resolution;
    td.Height     = resolution;
    td.MipLevels  = 1;
    td.ArraySize  = CSM_NUM_CASCADES;
    td.Format     = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc = { 1, 0 };
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    SE_HR(device->CreateTexture2D(&td, nullptr, &m_depthTex));

    // Per-cascade DSVs (one slice each)
    for (int i = 0; i < CSM_NUM_CASCADES; ++i)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = (UINT)i;
        dsvDesc.Texture2DArray.ArraySize       = 1;
        SE_HR(device->CreateDepthStencilView(m_depthTex.Get(), &dsvDesc, &m_dsv[i]));
    }

    // SRV: whole array
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                         = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MipLevels       = 1;
    srvDesc.Texture2DArray.ArraySize       = CSM_NUM_CASCADES;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    SE_HR(device->CreateShaderResourceView(m_depthTex.Get(), &srvDesc, &m_srv));

    // PCF comparison sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    SE_HR(device->CreateSamplerState(&sd, &m_shadowSampler));

    // Rasterizer state for shadow pass
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode              = D3D11_FILL_SOLID;
    rsDesc.CullMode              = D3D11_CULL_BACK;
    rsDesc.DepthBias             = 4000;
    rsDesc.DepthBiasClamp        = 0.0f;
    rsDesc.SlopeScaledDepthBias  = 2.0f;
    rsDesc.DepthClipEnable       = TRUE;
    SE_HR(device->CreateRasterizerState(&rsDesc, &m_shadowRS));

    // Unit sphere
    {
        std::vector<MeshVertex> verts;
        std::vector<uint32_t>   idx;
        BuildCSMSphere(1.0f, 16, 16, verts, idx);
        m_sphereVB.Create(device, verts.data(),
            static_cast<uint32_t>(verts.size() * sizeof(MeshVertex)), sizeof(MeshVertex));
        m_sphereIB.Create(device, idx.data(), static_cast<uint32_t>(idx.size()));
    }

    D3D11_SAMPLER_DESC alphaSamplerDesc = {};
    alphaSamplerDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    alphaSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    alphaSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    alphaSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    alphaSamplerDesc.MaxLOD   = D3D11_FLOAT32_MAX;
    SE_HR(device->CreateSamplerState(&alphaSamplerDesc, &m_alphaSampler));

    if (!m_cb.Create(device))         return false;
    if (!m_csmCB.Create(device))      return false;
    if (!m_materialCB.Create(device)) return false;

    SE_LOG_INFO("CSM: initialised %d cascades @ %ux%u", CSM_NUM_CASCADES, resolution, resolution);
    return true;
}

void CascadedShadowMap::Update(XMFLOAT3 lightDir,
                                XMMATRIX cameraView,
                                XMMATRIX cameraProj,
                                float cameraNear, float cameraFar)
{
    const bool orthographic = XMVectorGetW(cameraProj.r[3]) != 0.0f;
    if (orthographic)
    {
        const float depthScale = XMVectorGetZ(cameraProj.r[2]);
        const float depthOffset = XMVectorGetZ(cameraProj.r[3]);
        cameraNear = -depthOffset / depthScale;
        cameraFar = (1.0f - depthOffset) / depthScale;
    }
    // PSSM for perspective; linear slices for the parallel ortho volume,
    // including its negative near plane. Log splits cannot represent that.
    for (int i = 0; i <= CSM_NUM_CASCADES; ++i)
    {
        float p = (float)i / (float)CSM_NUM_CASCADES;
        float linSplit = cameraNear + (cameraFar - cameraNear) * p;
        float logSplit = orthographic ? linSplit : cameraNear * powf(cameraFar / cameraNear, p);
        m_splits[i] = splitLambda * logSplit + (1.0f - splitLambda) * linSplit;
    }

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    XMVECTOR up  = XMVectorSet(0, 1, 0, 0);
    if (fabsf(XMVectorGetY(dir)) > 0.99f)
        up = XMVectorSet(0, 0, 1, 0);

    // Inverse camera view-proj to unproject frustum corners
    XMMATRIX invView = XMMatrixInverse(nullptr, cameraView);

    // Get camera aspect and fov from projection matrix
    // proj[0][0] = 1/(aspect*tan(fov/2)), proj[1][1] = 1/tan(fov/2)
    float tanHalfFovY = 1.0f / XMVectorGetY(cameraProj.r[1]);
    float tanHalfFovX = 1.0f / XMVectorGetX(cameraProj.r[0]);

    for (int c = 0; c < CSM_NUM_CASCADES; ++c)
    {
        float nearZ = m_splits[c];
        float farZ  = m_splits[c + 1];

        // 8 frustum corners in view space
        XMFLOAT3 corners[8];
        float xn = tanHalfFovX, yn = tanHalfFovY;
        float xf = tanHalfFovX, yf = tanHalfFovY;
        if (!orthographic)
        {
            xn *= nearZ; yn *= nearZ;
            xf *= farZ; yf *= farZ;
        }
        corners[0] = { -xn, -yn, nearZ };
        corners[1] = {  xn, -yn, nearZ };
        corners[2] = {  xn,  yn, nearZ };
        corners[3] = { -xn,  yn, nearZ };
        corners[4] = { -xf, -yf, farZ  };
        corners[5] = {  xf, -yf, farZ  };
        corners[6] = {  xf,  yf, farZ  };
        corners[7] = { -xf,  yf, farZ  };

        // Transform to world space
        XMFLOAT3 worldCorners[8];
        XMVECTOR center = XMVectorZero();
        for (int i = 0; i < 8; ++i)
        {
            XMVECTOR v = XMVector3TransformCoord(XMLoadFloat3(&corners[i]), invView);
            XMStoreFloat3(&worldCorners[i], v);
            center = XMVectorAdd(center, v);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        float radius = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            XMVECTOR delta = XMVectorSubtract(XMLoadFloat3(&worldCorners[i]), center);
            radius = (std::max)(radius, XMVectorGetX(XMVector3Length(delta)));
        }

        // LightDir points from the scene toward the sun; render shadows from that side.
        XMVECTOR eye = XMVectorAdd(center, XMVectorScale(dir, radius * 2.0f));
        XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        // Find tight ortho bounds in light space.
        auto computeBounds = [&](float& minX, float& maxX, float& minY, float& maxY,
                                 float& minZ, float& maxZ) {
            minX = minY = minZ = FLT_MAX;
            maxX = maxY = maxZ = -FLT_MAX;
            for (int i = 0; i < 8; ++i)
            {
                XMVECTOR lv = XMVector3TransformCoord(XMLoadFloat3(&worldCorners[i]), lightView);
                float x = XMVectorGetX(lv), y = XMVectorGetY(lv), z = XMVectorGetZ(lv);
                minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
                minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
                minZ = (std::min)(minZ, z); maxZ = (std::max)(maxZ, z);
            }
        };

        float minX, maxX, minY, maxY, minZ, maxZ;
        computeBounds(minX, maxX, minY, maxY, minZ, maxZ);

        const float fitWidth = maxX - minX;
        const float fitHeight = maxY - minY;
        if (fitWidth > 1e-6f && fitHeight > 1e-6f)
        {
            // Snap the cascade centre to the light-space texel grid. Without
            // this the map slides by sub-texel offsets as the camera moves,
            // which reads as shadow shimmer under TAA/FSR.
            const float texelX = fitWidth / static_cast<float>(m_resolution);
            const float texelY = fitHeight / static_cast<float>(m_resolution);
            const XMVECTOR axisRight = lightView.r[0];
            const XMVECTOR axisUp    = lightView.r[1];
            const float centreRight = XMVectorGetX(XMVector3Dot(center, axisRight));
            const float centreUp    = XMVectorGetX(XMVector3Dot(center, axisUp));
            const float snappedRight = roundf(centreRight / texelX) * texelX;
            const float snappedUp    = roundf(centreUp / texelY) * texelY;
            const XMVECTOR snappedCenter = XMVectorAdd(center,
                XMVectorAdd(XMVectorScale(axisRight, snappedRight - centreRight),
                            XMVectorScale(axisUp, snappedUp - centreUp)));
            const XMVECTOR snappedEye =
                XMVectorAdd(snappedCenter, XMVectorScale(dir, radius * 2.0f));
            lightView = XMMatrixLookAtLH(snappedEye, snappedCenter, up);
            computeBounds(minX, maxX, minY, maxY, minZ, maxZ);
        }

        // Extend toward the sun to catch casters before the camera frustum slice.
        float zRange = maxZ - minZ;
        minZ -= zRange * 2.0f;

        XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            minX, maxX, minY, maxY, minZ, maxZ);

        m_cascadeVP[c] = XMMatrixMultiply(lightView, lightProj);
        m_texelWorld[c] = (maxX - minX) / (float)m_resolution;
    }
}

void CascadedShadowMap::BeginCascade(ID3D11DeviceContext* ctx, int cascade)
{
    m_currentCascade = cascade;

    if (cascade == 0)
    {
        // Save state only on first cascade
        UINT numVPs = 1;
        ctx->RSGetViewports(&numVPs, &m_savedVP);
        ctx->OMGetRenderTargets(1, &m_savedRTV, &m_savedDSV);
        ctx->RSGetState(&m_savedRS);
    }

    D3D11_VIEWPORT vp = {};
    vp.Width    = (float)m_resolution;
    vp.Height   = (float)m_resolution;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, m_dsv[cascade].Get());
    ctx->ClearDepthStencilView(m_dsv[cascade].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    ctx->RSSetState(m_shadowRS.Get());
    ctx->VSSetShader(m_perm->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_perm->ps.Get(), nullptr, 0);
    ctx->IASetInputLayout(m_layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetSamplers(0, 1, m_alphaSampler.GetAddressOf());
}

void CascadedShadowMap::DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh,
                                  XMMATRIX model,
                                  const std::vector<ShadowSubMesh>& subMeshes)
{
    ShadowCBData cb;
    XMStoreFloat4x4(&cb.model, model);
    XMStoreFloat4x4(&cb.viewProj, m_cascadeVP[m_currentCascade]);
    m_cb.Update(ctx, cb);
    m_cb.BindVS(ctx, 0);

    for (uint32_t i = 0; i < mesh.GetSubMeshCount(); ++i)
    {
        if (i < subMeshes.size() && !subMeshes[i].cast)
            continue;

        ShadowMaterialCBData material = {};
        if (i < subMeshes.size())
        {
            material.alphaCutoff = subMeshes[i].alphaCutoff;
            ID3D11ShaderResourceView* albedo = subMeshes[i].albedo;
            ctx->PSSetShaderResources(0, 1, &albedo);
        }
        m_materialCB.Update(ctx, material);
        m_materialCB.BindPS(ctx, 1);
        mesh.DrawSubMesh(ctx, i);
    }
}

void CascadedShadowMap::DrawSphere(ID3D11DeviceContext* ctx, XMFLOAT3 position, float radius)
{
    XMMATRIX model = XMMatrixScaling(radius, radius, radius) *
                     XMMatrixTranslation(position.x, position.y, position.z);
    ShadowCBData cb;
    XMStoreFloat4x4(&cb.model, model);
    XMStoreFloat4x4(&cb.viewProj, m_cascadeVP[m_currentCascade]);
    m_cb.Update(ctx, cb);
    m_cb.BindVS(ctx, 0);

    m_sphereVB.Bind(ctx);
    m_sphereIB.Bind(ctx);
    ctx->DrawIndexed(m_sphereIB.GetCount(), 0, 0);
}

void CascadedShadowMap::EndCascade(ID3D11DeviceContext* ctx)
{
    // Only restore on the last cascade
    if (m_currentCascade == CSM_NUM_CASCADES - 1)
    {
        ctx->OMSetRenderTargets(1, &m_savedRTV, m_savedDSV);
        ctx->RSSetViewports(1, &m_savedVP);
        ctx->RSSetState(m_savedRS);
        if (m_savedRTV) { m_savedRTV->Release(); m_savedRTV = nullptr; }
        if (m_savedDSV) { m_savedDSV->Release(); m_savedDSV = nullptr; }
        if (m_savedRS)  { m_savedRS->Release();  m_savedRS  = nullptr; }
    }
}

void CascadedShadowMap::BindForLitPass(ID3D11DeviceContext* ctx)
{
    // Update CSM cbuffer
    CSMCBData data = {};
    for (int i = 0; i < CSM_NUM_CASCADES; ++i)
        XMStoreFloat4x4(&data.cascadeVP[i], m_cascadeVP[i]);
    // Store view-space split distances (far plane of each cascade)
    for (int i = 0; i < CSM_NUM_CASCADES; ++i)
        data.splitDistances[i] = m_splits[i + 1];
    data.splitDistances[3] = 0.0f; // padding
    for (int i = 0; i < CSM_NUM_CASCADES; ++i)
        data.texelSizes[i] = m_texelWorld[i];
    data.texelSizes[3] = 0.0f; // padding

    m_csmCB.Update(ctx, data);
    m_csmCB.BindPS(ctx, 6);

    // Bind shadow array and sampler
    ID3D11ShaderResourceView* srv = m_srv.Get();
    ctx->PSSetShaderResources(3, 1, &srv);
    ctx->PSSetSamplers(1, 1, m_shadowSampler.GetAddressOf());
}

void CascadedShadowMap::Unbind(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(3, 1, &nullSrv);
}

} // namespace SE
