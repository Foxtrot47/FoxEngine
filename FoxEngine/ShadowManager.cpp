#include "ShadowManager.h"
#include "LightManager.h"
#include "FileUtils.h"

ShadowManager::ShadowManager(Graphics& gfx)
	:
	viewProjectionMatrix(DirectX::XMMATRIX())
{
	HRESULT hr = E_FAIL;
	D3D11_TEXTURE2D_DESC shadowMapDesc = {};
	shadowMapDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	shadowMapDesc.MipLevels = 1;
	shadowMapDesc.ArraySize = 1;
	shadowMapDesc.SampleDesc.Count = 1;
	shadowMapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	shadowMapDesc.Height = static_cast<UINT>(2048);
	shadowMapDesc.Width = static_cast<UINT>(2048);

	hr = gfx.pDevice->CreateTexture2D(
		&shadowMapDesc,
		nullptr,
		&pShadowMapTexture
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create shadow map texture");
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
	depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	depthStencilViewDesc.Texture2D.MipSlice = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
	shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderResourceViewDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	shaderResourceViewDesc.Texture2D.MipLevels = 1;

	D3D11_DEPTH_STENCIL_DESC descDepthStencil;
	ZeroMemory(&descDepthStencil, sizeof(D3D11_DEPTH_STENCIL_DESC));
	descDepthStencil.DepthEnable = TRUE;
	descDepthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	descDepthStencil.DepthFunc = D3D11_COMPARISON_LESS;

	hr = gfx.pDevice->CreateDepthStencilState(&descDepthStencil, &pDepthStencilState);

	hr = gfx.pDevice->CreateDepthStencilView(
		pShadowMapTexture.Get(),
		&depthStencilViewDesc,
		&pShadowDepthView
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create shadow map depth stencil view");
	}

	hr = gfx.pDevice->CreateShaderResourceView(
		pShadowMapTexture.Get(),
		&shaderResourceViewDesc,
		&pShadowMapSRV
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create shadow map shader resource view");
	}

	D3D11_SAMPLER_DESC comparisonSamplerDesc = {};
	comparisonSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
	comparisonSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
	comparisonSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
	comparisonSamplerDesc.BorderColor[0] = 1.0f;
	comparisonSamplerDesc.BorderColor[1] = 1.0f;
	comparisonSamplerDesc.BorderColor[2] = 1.0f;
	comparisonSamplerDesc.BorderColor[3] = 1.0f;
	comparisonSamplerDesc.MinLOD = 0.f;
	comparisonSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	comparisonSamplerDesc.MipLODBias = 0.f;
	comparisonSamplerDesc.MaxAnisotropy = 0;
	comparisonSamplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	comparisonSamplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;

	hr = gfx.pDevice->CreateSamplerState(
		&comparisonSamplerDesc,
		&pComparisonSampler
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create comparison sampler state");
	}

	D3D11_RASTERIZER_DESC shadowRenderStateDesc = {};
	shadowRenderStateDesc.CullMode = D3D11_CULL_FRONT;
	shadowRenderStateDesc.FillMode = D3D11_FILL_SOLID;
	shadowRenderStateDesc.DepthClipEnable = true;

	hr = gfx.pDevice->CreateRasterizerState(
		&shadowRenderStateDesc,
		&pShadowRasterizerState
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create rasterizer state");
	}

	shadowViewPort.Width = 2048;
	shadowViewPort.Height = 2048;
	shadowViewPort.MinDepth = 0.f;
	shadowViewPort.MaxDepth = 1.f;

	const auto shaderPath = GetShaderPath(L"ShadowMapVS.cso");
	hr = D3DReadFileToBlob(shaderPath.c_str(), &pShadowVSByteCode);

	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(shaderPath.begin(), shaderPath.end()));
	}

	hr = gfx.pDevice->CreateVertexShader(
		pShadowVSByteCode->GetBufferPointer(),
		pShadowVSByteCode->GetBufferSize(),
		nullptr,
		&pShadowVS
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create vertex shader");
	}

	topologyDesc =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};

	hr = gfx.pDevice->CreateInputLayout(
		topologyDesc.data(),
		(UINT)topologyDesc.size(),
		pShadowVSByteCode->GetBufferPointer(),
		pShadowVSByteCode->GetBufferSize(),
		&pInputLayout
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create input layout");
	}
}

void ShadowManager::BeginShadowPass(Graphics& gfx) const
{
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	gfx.pContext->PSSetShaderResources(4, 1, nullSRV);
	gfx.pContext->OMSetRenderTargets(0u, nullptr, pShadowDepthView.Get());
	gfx.pContext->RSSetState(pShadowRasterizerState.Get());
	gfx.pContext->RSSetViewports(1, &shadowViewPort);
	gfx.pContext->OMSetDepthStencilState(pDepthStencilState.Get(), 1u);
	gfx.pContext->ClearDepthStencilView(
		pShadowDepthView.Get(),
		D3D11_CLEAR_DEPTH,
		1.0f,
		0u
	);
	gfx.pContext->VSSetShader(pShadowVS.Get(), nullptr, 0u);
	gfx.pContext->PSSetShader(nullptr, nullptr, 0u);
	gfx.pContext->IASetInputLayout(pInputLayout.Get());
}

DirectX::XMMATRIX ShadowManager::GetLightViewProj(Graphics& gfx)
{
	return gfx.GetLightManager()->CalculateLightMatrix(0);
}

void ShadowManager::Bind(Graphics& gfx) const
{
	// shadowmap binded to 4th slot
	gfx.pContext->PSSetShaderResources(4u, 1u, pShadowMapSRV.GetAddressOf());
	gfx.pContext->PSSetSamplers(1u, 1u, pComparisonSampler.GetAddressOf());
}