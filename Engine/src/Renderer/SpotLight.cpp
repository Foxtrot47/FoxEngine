#include "Engine/Renderer/SpotLight.h"
#include "Engine/Core/Logger.h"
#include <cmath>

using namespace DirectX;

namespace SE {

bool SpotLight::Init(ID3D11Device* device, ShaderLibrary& shaders, uint32_t resolution)
{
    m_resolution = resolution;

    // Depth texture (typeless for DSV + SRV)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width     = resolution;
    td.Height    = resolution;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, m_depthTex.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("SpotLight: depth texture failed"); return false; }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = device->CreateDepthStencilView(m_depthTex.Get(), &dsvDesc, m_dsv.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("SpotLight: DSV failed"); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    hr = device->CreateShaderResourceView(m_depthTex.Get(), &srvDesc, m_srv.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("SpotLight: SRV failed"); return false; }

    // Shadow rasterizer (depth bias to reduce acne)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_BACK;
    rd.DepthBias       = 1000;
    rd.SlopeScaledDepthBias = 2.0f;
    rd.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rd, m_shadowRS.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("SpotLight: rasterizer state failed"); return false; }

    // Reuse the existing shadow depth shader
    m_shadowPerm = shaders.Get(L"Shaders/ShadowDepth.hlsl");
    if (!m_shadowPerm) { SE_LOG_ERROR("SpotLight: ShadowDepth.hlsl not found"); return false; }

    // Create input layout for shadow pass (must match MeshVertex stride)
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(
        layoutDesc, 5,
        m_shadowPerm->vsBlob->GetBufferPointer(),
        m_shadowPerm->vsBlob->GetBufferSize(),
        m_inputLayout.GetAddressOf());
    if (FAILED(hr)) { SE_LOG_ERROR("SpotLight: input layout failed"); return false; }

    if (!m_shadowCB.Create(device)) return false;
    if (!m_lightCB.Create(device)) return false;

    SE_LOG_INFO("SpotLight: initialised %ux%u shadow map", resolution, resolution);
    return true;
}

void SpotLight::Update()
{
    XMVECTOR pos = XMLoadFloat3(&position);
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));

    // Build an up vector that isn't parallel to direction
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetY(dir)) > 0.99f)
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookToLH(pos, dir, up);

    // Perspective projection covering the outer cone angle
    float fovRad = XMConvertToRadians(outerAngle * 2.0f);
    float nearZ  = range * 0.01f;  // 1% of range avoids depth precision issues
    if (nearZ < 0.5f) nearZ = 0.5f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, 1.0f, nearZ, range);

    m_viewProj = XMMatrixMultiply(view, proj);
}

void SpotLight::BeginShadowPass(ID3D11DeviceContext* ctx)
{
    Update();

    // Save current state
    UINT numVP = 1;
    ctx->RSGetViewports(&numVP, &m_savedVP);
    ctx->OMGetRenderTargets(1, &m_savedRTV, &m_savedDSV);
    ctx->RSGetState(&m_savedRS);

    // Clear and bind shadow depth target
    ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, m_dsv.Get());

    D3D11_VIEWPORT vp = {};
    vp.Width    = (float)m_resolution;
    vp.Height   = (float)m_resolution;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(m_shadowRS.Get());

    ctx->VSSetShader(m_shadowPerm->vs.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(m_inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void SpotLight::DrawMesh(ID3D11DeviceContext* ctx, const Mesh& mesh, XMMATRIX model)
{
    ShadowCBData cb;
    XMStoreFloat4x4(&cb.model, model);
    XMStoreFloat4x4(&cb.viewProj, m_viewProj);
    m_shadowCB.Update(ctx, cb);
    m_shadowCB.BindVS(ctx, 0);

    mesh.Draw(ctx);
}

void SpotLight::EndShadowPass(ID3D11DeviceContext* ctx)
{
    ctx->OMSetRenderTargets(1, &m_savedRTV, m_savedDSV);
    ctx->RSSetViewports(1, &m_savedVP);
    ctx->RSSetState(m_savedRS);

    if (m_savedRTV) { m_savedRTV->Release(); m_savedRTV = nullptr; }
    if (m_savedDSV) { m_savedDSV->Release(); m_savedDSV = nullptr; }
    if (m_savedRS)  { m_savedRS->Release();  m_savedRS = nullptr; }
}

void SpotLight::BindForLitPass(ID3D11DeviceContext* ctx)
{
    // Update the spot light cbuffer
    SpotLightCBData data = {};
    data.position   = position;
    data.range      = range;
    data.direction  = direction;
    data.outerCos   = cosf(XMConvertToRadians(outerAngle));
    data.color      = { color.x * intensity, color.y * intensity, color.z * intensity };
    data.innerCos   = cosf(XMConvertToRadians(innerAngle));
    XMStoreFloat4x4(&data.viewProj, m_viewProj);
    data.shadowBias  = shadowBias;
    data.spotEnabled = enabled ? 1 : 0;
    m_lightCB.Update(ctx, data);
    m_lightCB.BindPS(ctx, 5);

    // Bind shadow map to t8
    ctx->PSSetShaderResources(8, 1, m_srv.GetAddressOf());
}

void SpotLight::Unbind(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* null = nullptr;
    ctx->PSSetShaderResources(8, 1, &null);
}

} // namespace SE
