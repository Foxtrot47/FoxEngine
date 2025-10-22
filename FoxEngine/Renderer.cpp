#include "Renderer.h"
#include "FileUtils.h"
#include "Material.h"
#include "MeshNode.h"

Renderer::Renderer(
	Graphics& gfx,
	FPVCamera& camera,
	int windowWidth,
	int windowHeight
)
	: gfx(gfx),
	camera(camera)
{
	InitializeColorPass(windowWidth, windowHeight);
	InitializeShadowPass();
}

void Renderer::CollectFrameData(SceneNode& node)
{
	nodes.clear();
	nodes.reserve(1024);
	TraverseAndCollect(node);
}

void Renderer::BeginFrame(float red, float green, float blue)
{
	const float clearColor[4] = { red, green, blue, 1.0f }; // RGBA color
	gfx.pContext->ClearRenderTargetView(pBackBufferRTV.Get(), clearColor); // Clear the render target view
	gfx.pContext->ClearDepthStencilView(
		pDepthStencilView.Get(),
		D3D11_CLEAR_DEPTH, // Clear depth and stencil
		1.0f, // Depth value to clear to
		0u // Stencil value to clear to
	); // Clear the depth stencil view

	if (gfx.GetLightManager()->HasDirectionalLight())
	{
		gfx.pContext->ClearDepthStencilView(
			pDirectionalShadowDepthView.Get(),
			D3D11_CLEAR_DEPTH,
			1.0f,
			0u
		);
	}

	for (int i = 0; i < pointShadowMapSRVs.size(); i++)
	{
		gfx.pContext->ClearDepthStencilView(
			pPointShadowDepthView[i].Get(),
			D3D11_CLEAR_DEPTH,
			1.0f,
			0u
		);
	}
	gfx.BeginFrame();
}

void Renderer::RenderScene()
{
	RendeShadowPass();
	RenderColorPass();
}

void Renderer::RenderColorPass()
{
	gfx.pContext->OMSetRenderTargets(1u, pBackBufferRTV.GetAddressOf(), pDepthStencilView.Get());
	gfx.pContext->RSSetState(pRasterizerState.Get());
	gfx.pContext->RSSetViewports(1, &renderViewPort);
	gfx.BeginRenderPass();

	// Bind shadow map to 4th slot
	std::vector<ID3D11ShaderResourceView*> rawSRV(pointShadowMapSRVs.size());
	for (int i=0; i< pointShadowMapSRVs.size(); i++)
	{
		rawSRV[i] = pointShadowMapSRVs[i].Get();
	}
	gfx.pContext->PSSetShaderResources(4u, static_cast<UINT>(rawSRV.size()), rawSRV.data());

	// directional shadows goes to 9th slot
	if (gfx.GetLightManager()->HasDirectionalLight())
	{
		gfx.pContext->PSSetShaderResources(9u, 1u, pDirectionalShadowSRV.GetAddressOf());
	}

	gfx.pContext->PSSetSamplers(1u, 1u, pComparisonSampler.GetAddressOf());

	currentVertexShaderPath.clear();
	currentPixelShaderPath.clear();

	BuildMaterialBatches();

	for (const auto& batchPair : materialBatches)
	{
		Material* material = batchPair.first;
		const std::vector<MeshTransformPair>& meshes = batchPair.second;
		RenderBatch(material, meshes);
	}
}

void Renderer::RendeShadowPass()
{	// set pixel shader to null
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	gfx.pContext->PSSetShaderResources(4, 1, nullSRV);
	gfx.pContext->PSSetShaderResources(9, 1, nullSRV);
	gfx.pContext->PSSetShader(nullptr, nullptr, 0u);
	gfx.pContext->VSSetShader(pShadowVS.Get(), nullptr, 0u);

	gfx.pContext->RSSetState(pShadowRasterizerState.Get());
	gfx.pContext->RSSetViewports(1, &shadowViewPort);
	gfx.pContext->OMSetDepthStencilState(pShadowDepthStencilState.Get(), 1u);
	gfx.pContext->IASetInputLayout(pShadowInputLayout.Get());

	if (gfx.GetLightManager()->HasDirectionalLight())
	{
		auto lightViewProj = gfx.GetLightManager()->CalculateDirectionalLightMatrix();
		gfx.pContext->OMSetRenderTargets(0u, nullptr, pDirectionalShadowDepthView.Get());

		for (MeshNode* node : nodes)
		{
			auto& meshes = node->GetMeshes();
			const auto transform = node->GetWorldTransform();

			for (auto& mesh : meshes)
			{
				const ShadowConstants shadowTransforms = {
					DirectX::XMMatrixTranspose(transform * lightViewProj)
				};

				shadowConstantBuffer->Update(gfx, shadowTransforms);
				shadowConstantBuffer->Bind(gfx);

				mesh->GetVertexBuffer()->Bind(gfx);
				mesh->GetIndexBuffer()->Bind(gfx);
				mesh->GetTopology()->Bind(gfx);
				gfx.DrawIndexed(mesh->GetIndexBuffer()->GetCount());
			}
		}
	}

	const int activeLights = gfx.GetLightManager()->GetActiveLights();

	for (int i = 0; i < activeLights; i++)
	{
		auto lightViewProj = gfx.GetLightManager()->CalculatePointLightMatrix(i);
		gfx.pContext->OMSetRenderTargets(0u, nullptr, pPointShadowDepthView[i].Get());

		for (MeshNode* node : nodes)
		{
			auto& meshes = node->GetMeshes();
			const auto transform = node->GetWorldTransform();

			for (auto& mesh : meshes)
			{
				const ShadowConstants shadowTransforms = {
					DirectX::XMMatrixTranspose(transform * lightViewProj)
				};

				shadowConstantBuffer->Update(gfx, shadowTransforms);
				shadowConstantBuffer->Bind(gfx);

				mesh->GetVertexBuffer()->Bind(gfx);
				mesh->GetIndexBuffer()->Bind(gfx);
				mesh->GetTopology()->Bind(gfx);
				gfx.DrawIndexed(mesh->GetIndexBuffer()->GetCount());
			}
		}
	}
}

void Renderer::EndFrame()
{
	gfx.EndFrame();
}

void Renderer::TraverseAndCollect(SceneNode& node)
{
	for (const auto& child : node.GetChildren())
	{
		if (auto* meshNode = dynamic_cast<MeshNode*>(child.get()))
		{
			nodes.push_back(meshNode);
		}
		TraverseAndCollect(*child);
	}
}

void Renderer::InitializeColorPass(int windowWidth, int windowHeight)
{
	HRESULT hr = E_FAIL;

	hr = gfx.pSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), &pBackBuffer);
	// gain access to texture subresource of the swap chain (back buffer)
	if (FAILED(hr) || !pBackBuffer) {
		throw new std::runtime_error("Failed to capture back buffer");
	}

	gfx.pDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &pBackBufferRTV); // Create a render target view  

	D3D11_DEPTH_STENCIL_DESC descDepthStencil = {};
	descDepthStencil.DepthEnable = TRUE;
	descDepthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	descDepthStencil.DepthFunc = D3D11_COMPARISON_LESS;

	hr = gfx.pDevice->CreateDepthStencilState(&descDepthStencil, &pDepthStencilState);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create depth stencil state");
	}
	gfx.pContext->OMSetDepthStencilState(pDepthStencilState.Get(), 1u); // Set the depth stencil state

	// create depth stencil texture
	D3D11_TEXTURE2D_DESC descDepth = {};
	descDepth.Width = windowWidth;   // Set the width of the depth stencil buffer
	descDepth.Height = windowHeight; // Set the height of the depth stencil buffer
	descDepth.MipLevels = 1u; // No mipmaps
	descDepth.ArraySize = 1u; // Single texture
	descDepth.Format = DXGI_FORMAT_D32_FLOAT; // 32-bit depth buffer
	descDepth.SampleDesc.Count = 1; // No multisampling
	descDepth.SampleDesc.Quality = 0u; // No multisampling quality
	descDepth.Usage = D3D11_USAGE_DEFAULT;
	descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL; // Bind as a depth stencil buffer
	gfx.pDevice->CreateTexture2D(&descDepth, nullptr, &pDepthStencilTexture);	// Create the depth stencil buffer

	// create view of the depth stencil texture
	D3D11_DEPTH_STENCIL_VIEW_DESC descDepthStencilView = {};
	descDepthStencilView.Format = DXGI_FORMAT_D32_FLOAT;
	descDepthStencilView.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	descDepthStencilView.Texture2D.MipSlice = 0; // Use the first mip level
	gfx.pDevice->CreateDepthStencilView(
		pDepthStencilTexture.Get(),
		&descDepthStencilView,
		&pDepthStencilView
	);

	D3D11_RASTERIZER_DESC rsDesc = {};
	rsDesc.FillMode = D3D11_FILL_SOLID;          // solid rendering (wireframe = D3D11_FILL_WIREFRAME)
	rsDesc.CullMode = D3D11_CULL_BACK;           // cull backfaces
	rsDesc.FrontCounterClockwise = FALSE;        // DX default = clockwise is front
	rsDesc.DepthClipEnable = TRUE;               // clip primitives outside near/far planes

	hr = gfx.pDevice->CreateRasterizerState(&rsDesc, &pRasterizerState);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create rasterizer state");
	}

	// configure viewport
	renderViewPort.Width = windowWidth;						// Set viewport width
	renderViewPort.Height = windowHeight;					// Set viewport height
	renderViewPort.MinDepth = 0;							// Minimum depth
	renderViewPort.MaxDepth = 1;							// Maximum depth
	renderViewPort.TopLeftX = 0;							// Top-left X position
	renderViewPort.TopLeftY = 0;							// Top-left Y position


	if (!std::filesystem::exists(GetShaderPath(phongVS))) {
		throw std::runtime_error("Shader file not found: " + std::string(GetShaderPath(phongVS).begin(), GetShaderPath(phongVS).end()));
	}

	hr = D3DReadFileToBlob(GetShaderPath(phongVS).c_str(), &pShaderBlob);

	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(GetShaderPath(phongVS).begin(), GetShaderPath(phongVS).end()));
	}

	hr = gfx.pDevice->CreateVertexShader(
		pShaderBlob->GetBufferPointer(),
		pShaderBlob->GetBufferSize(),
		nullptr,
		&pPhongVS
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create vertex shader: " + std::string(GetShaderPath(phongVS).begin(), GetShaderPath(phongVS).end()));
	}

	if (!std::filesystem::exists(GetShaderPath(phongPS))) {
		throw std::runtime_error("Shader file not found: " + std::string(GetShaderPath(phongPS).begin(), GetShaderPath(phongPS).end()));
	}

	hr = D3DReadFileToBlob(GetShaderPath(phongPS).c_str(), &pShaderBlob);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read pixel shader file: " + std::string(GetShaderPath(phongPS).begin(), GetShaderPath(phongPS).end()));
	}

	hr = gfx.pDevice->CreatePixelShader(
		pShaderBlob->GetBufferPointer(),
		pShaderBlob->GetBufferSize(),
		nullptr,
		&pPhongPS
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create pixel shader: " + std::string(GetShaderPath(phongPS).begin(), GetShaderPath(phongPS).end()));
	}
}

void Renderer::InitializeShadowPass()
{
	HRESULT hr = E_FAIL;
	D3D11_TEXTURE2D_DESC pointShadowMapDesc = {};
	pointShadowMapDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	pointShadowMapDesc.MipLevels = 1;
	pointShadowMapDesc.ArraySize = 1;
	pointShadowMapDesc.SampleDesc.Count = 1;
	pointShadowMapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	pointShadowMapDesc.Height = static_cast<UINT>(2048);
	pointShadowMapDesc.Width = static_cast<UINT>(2048);

	D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
	depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	depthStencilViewDesc.Texture2D.MipSlice = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceViewDesc = {};
	shaderResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderResourceViewDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	shaderResourceViewDesc.Texture2D.MipLevels = 1;

	for (int i = 0; i < pointShadowMapSRVs.size(); i++)
	{
		hr = gfx.pDevice->CreateTexture2D(
			&pointShadowMapDesc,
			nullptr,
			&pPointShadowTexture[i]
		);

		if (FAILED(hr))
		{
			throw std::runtime_error("Failed to create point shadow map texture");
		}

		hr = gfx.pDevice->CreateDepthStencilView(
			pPointShadowTexture[i].Get(),
			&depthStencilViewDesc,
			&pPointShadowDepthView[i]
		);

		if (FAILED(hr))
		{
			throw std::runtime_error("Failed to create shadow map depth stencil view");
		}

		hr = gfx.pDevice->CreateShaderResourceView(
			pPointShadowTexture[i].Get(),
			&shaderResourceViewDesc,
			&pointShadowMapSRVs[i]
		);
		if (FAILED(hr))
		{
			throw std::runtime_error("Failed to create shadow map SRV");
		}
	}

	D3D11_TEXTURE2D_DESC directionalShadowMapDesc = {};
	directionalShadowMapDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	directionalShadowMapDesc.MipLevels = 1;
	directionalShadowMapDesc.ArraySize = 1;
	directionalShadowMapDesc.SampleDesc.Count = 1;
	directionalShadowMapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	directionalShadowMapDesc.Height = static_cast<UINT>(8192);
	directionalShadowMapDesc.Width = static_cast<UINT>(8192);

	hr = gfx.pDevice->CreateTexture2D(
		&directionalShadowMapDesc,
		nullptr,
		&pDirectionalShadowTexture
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create directional shadow map texture");
	}

	hr = gfx.pDevice->CreateDepthStencilView(
		pDirectionalShadowTexture.Get(),
		&depthStencilViewDesc,
		&pDirectionalShadowDepthView
	);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create directional shadow map depth stencil view");
	}

	hr = gfx.pDevice->CreateShaderResourceView(
		pDirectionalShadowTexture.Get(),
		&shaderResourceViewDesc,
		&pDirectionalShadowSRV
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create directional shadow map SRV");
	}

	D3D11_DEPTH_STENCIL_DESC descDepthStencil = {};
	descDepthStencil.DepthEnable = TRUE;
	descDepthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	descDepthStencil.DepthFunc = D3D11_COMPARISON_LESS;

	hr = gfx.pDevice->CreateDepthStencilState(&descDepthStencil, &pShadowDepthStencilState);

	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create shadow depth stencil state");
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
		throw std::runtime_error("Failed to create shadow rasterizer state");
	}

	shadowViewPort.Width = 8192;
	shadowViewPort.Height = 8192;
	shadowViewPort.MinDepth = 0.f;
	shadowViewPort.MaxDepth = 1.f;

	const auto shaderPath = GetShaderPath(L"ShadowMapVS.cso");
	hr = D3DReadFileToBlob(shaderPath.c_str(), &pShaderBlob);

	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(shaderPath.begin(), shaderPath.end()));
	}

	hr = gfx.pDevice->CreateVertexShader(
		pShaderBlob->GetBufferPointer(),
		pShaderBlob->GetBufferSize(),
		nullptr,
		&pShadowVS
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create vertex shader");
	}

	std::vector<D3D11_INPUT_ELEMENT_DESC> shadowTopologyDesc =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};

	hr = gfx.pDevice->CreateInputLayout(
		shadowTopologyDesc.data(),
		(UINT)shadowTopologyDesc.size(),
		pShaderBlob->GetBufferPointer(),
		pShaderBlob->GetBufferSize(),
		&pShadowInputLayout
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create shadow input layout");
	}

	shadowConstantBuffer = std::make_unique<VertexConstantBuffer<ShadowConstants>>(gfx, 0u);
}

void Renderer::BindShaderForMaterial(Material* material)
{
	std::wstring desiredVSPath;
	std::wstring desiredPSPath;

	if (material && material->HasShaderOverride())
	{
		desiredVSPath = material->GetVertexShaderPath();
		desiredPSPath = material->GetPixelShaderPath();
	}

	// Bind vertex shader only if changed
	if (currentVertexShaderPath != desiredVSPath)
	{
		if (!desiredVSPath.empty())
		{
			// Load custom vertex shader if not cached
			if (vertexShaderCache.find(desiredVSPath) == vertexShaderCache.end())
			{
				vertexShaderCache[desiredVSPath] = LoadVertexShader(desiredVSPath, pShaderBlob);

				if (inputLayoutCache.find(desiredVSPath) == inputLayoutCache.end())
				{
					inputLayoutCache[desiredVSPath] = CreateInputLayout(pShaderBlob);
				}
			}
			gfx.pContext->VSSetShader(vertexShaderCache[desiredVSPath].Get(), nullptr, 0u);
			gfx.pContext->IASetInputLayout(inputLayoutCache[desiredVSPath].Get());
		}
		else
		{
			// Use default vertex shader
			gfx.pContext->VSSetShader(pPhongVS.Get(), nullptr, 0u);
			gfx.pContext->IASetInputLayout(pInputLayout.Get());
		}
		currentVertexShaderPath = desiredVSPath;
	}
	if (currentPixelShaderPath != desiredPSPath)
	{
		if (!desiredPSPath.empty())
		{
			// Load custom pixel shader if not cached
			if (pixelShaderCache.find(desiredPSPath) == pixelShaderCache.end())
			{
				pixelShaderCache[desiredPSPath] = LoadPixelShader(desiredPSPath);
			}
			gfx.pContext->PSSetShader(pixelShaderCache[desiredPSPath].Get(), nullptr, 0u);
		}
		else
		{
			// Use default pixel shader
			gfx.pContext->PSSetShader(pPhongPS.Get(), nullptr, 0u);
		}
		currentPixelShaderPath = desiredPSPath;
	}
}

ComPtr<ID3D11VertexShader> Renderer::LoadVertexShader(const std::wstring& path, ComPtr<ID3DBlob>& outBlob)
{
	ComPtr<ID3D11VertexShader> vertexShader;

	if (!std::filesystem::exists(path)) {
		throw std::runtime_error("Shader file not found: " + std::string(path.begin(), path.end()));
	}

	HRESULT hr = D3DReadFileToBlob(path.c_str(), &outBlob);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read vertex shader file: " + std::string(path.begin(), path.end()));
	}

	hr = gfx.pDevice->CreateVertexShader(
		outBlob->GetBufferPointer(),
		outBlob->GetBufferSize(),
		nullptr,
		&vertexShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create vertex shader: " + std::string(path.begin(), path.end()));
	}

	return vertexShader;
}

ComPtr<ID3D11PixelShader> Renderer::LoadPixelShader(const std::wstring& path)
{
	ComPtr<ID3D11PixelShader> pixelShader;

	if (!std::filesystem::exists(path)) {
		throw std::runtime_error("Shader file not found: " + std::string(path.begin(), path.end()));
	}

	HRESULT hr = D3DReadFileToBlob(path.c_str(), &pShaderBlob);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to read pixel shader file: " + std::string(path.begin(), path.end()));
	}

	hr = gfx.pDevice->CreatePixelShader(
		pShaderBlob->GetBufferPointer(),
		pShaderBlob->GetBufferSize(),
		nullptr,
		&pixelShader
	);
	if (FAILED(hr)) {
		throw std::runtime_error("Failed to create pixel shader: " + std::string(path.begin(), path.end()));
	}

	return pixelShader;
}

ComPtr<ID3D11InputLayout> Renderer::CreateInputLayout(ComPtr<ID3DBlob> vsBlob)
{
	ComPtr<ID3D11InputLayout> layout;

	std::vector<D3D11_INPUT_ELEMENT_DESC> topologyDesc =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "Normal", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TexCoord", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "Tangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BiTangent", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};

	HRESULT hr = gfx.pDevice->CreateInputLayout(
		topologyDesc.data(),
		(UINT)topologyDesc.size(),
		vsBlob->GetBufferPointer(),
		vsBlob->GetBufferSize(),
		&layout
	);
	if (FAILED(hr))
	{
		throw std::runtime_error("Failed to create input layout");
	}

	return layout;
}

void Renderer::RenderBatch(Material* material, const std::vector<MeshTransformPair>& meshes)
{
	BindShaderForMaterial(material);
	material->Bind(gfx);

	for (const auto& meshPair : meshes)
	{
		Mesh* mesh = meshPair.first;
		DirectX::XMMATRIX transform = meshPair.second;
		mesh->GetVertexBuffer()->Bind(gfx);
		mesh->GetIndexBuffer()->Bind(gfx);
		mesh->GetTopology()->Bind(gfx);
		mesh->GetTransformCB()->Bind(gfx, transform);

		gfx.DrawIndexed(mesh->GetIndexBuffer()->GetCount());
	}
}

void Renderer::BuildMaterialBatches()
{
	materialBatches.clear();

	for (MeshNode* node : nodes)
	{
		const auto& meshes = node->GetMeshes();
		const auto transform = node->GetWorldTransform();

		for (const auto& mesh : meshes)
		{
			Material* material = mesh->GetMaterial();
			materialBatches[material].emplace_back(mesh.get(), transform);
		}
	}
}
