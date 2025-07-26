#include "Graphics.h"
#include <DirectxMath.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCompiler.lib")

Graphics::Graphics(HWND hWnd) : pDevice(nullptr), pSwapChain(nullptr), pContext(nullptr)
{
	// Create a struct to hold information about the swap chain
	DXGI_SWAP_CHAIN_DESC scd;
	ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

	// Fill the swap chain description struct
	scd.BufferCount = 1;                                   // One back buffer
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;    // Use 32-bit color
	scd.BufferDesc.Width = 0;                              // Set the back buffer width
	scd.BufferDesc.Height = 0;                             // Set the back buffer height
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;     // How the swap chain is to be used
	scd.OutputWindow = hWnd;                               // The window to be used
	scd.SampleDesc.Count = 4;                              // How many multisamples
	scd.Windowed = TRUE;                                   // Windowed/full-screen mode
	scd.Flags = 0;                                         // No special flags
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;             // Discard the old back buffer when presenting

	// Create a device, device context and swap chain using the information in the scd struct
	D3D11CreateDeviceAndSwapChain(
		nullptr,                    // Default adapter
		D3D_DRIVER_TYPE_HARDWARE,   // Use hardware rendering
		nullptr,                    // No software device
		D3D11_CREATE_DEVICE_DEBUG,	// No flags
		nullptr,                    // Default feature level array
		0,                          // Default feature level array size
		D3D11_SDK_VERSION,          // Always set this to D3D11_SDK_VERSION
		&scd,                       // The swap chain description
		&pSwapChain,                // The swap chain
		&pDevice,                   // The device
		nullptr,                    // The feature level
		&pContext                   // The device context
	);

	// gain access to texture subresource of the swap chain (back buffer)
	Microsoft::WRL::ComPtr<ID3D11Resource> pBackBuffer = nullptr;
	if (pSwapChain) {
		HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), &pBackBuffer);
		if (SUCCEEDED(hr) && pBackBuffer) {
			pDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &pTarget); // Create a render target view  
		}
	}
}

void Graphics::EndFrame()
{
	// Present the back buffer to the front buffer
	if (pSwapChain) {
		pSwapChain->Present(1, 0); // 1 for vsync, 0 for no flags
	}
}

void Graphics::ClearBuffer(float red, float green, float blue)
{
	// Clear the back buffer to a specific color
	if (pContext) {
		const float clearColor[4] = { red, green, blue, 1.0f }; // RGBA color
		pContext->ClearRenderTargetView(pTarget.Get(), clearColor); // Clear the render target view
	}
}

void Graphics::DrawTriangle(float angle)
{
	Microsoft::WRL::ComPtr<ID3D11Buffer> pVertexBuffer = nullptr;

	D3D11_BUFFER_DESC bufferDesc;
	ZeroMemory(&bufferDesc, sizeof(D3D11_BUFFER_DESC));

	struct Vertex
	{
		struct
		{
			float x;
			float y;
		} pos;
		struct
		{
			unsigned char r;
			unsigned char g;
			unsigned char b;
			unsigned char a;
		} color;
	};

	const Vertex vertices[] = {
		{ 0.0f,0.5f,255,0,0,0 },
		{ 0.5f,-0.5f,0,255,0,0},
		{ -0.5f,-0.5f,0,0,255,0 },
		{ -0.3f, 0.3f,0,255,0,0},
		{ 0.3f,0.3f,0,0,255,0},
		{ 0.0f,-0.8f,255,0,0,0}
	};

	bufferDesc.Usage = D3D11_USAGE_DEFAULT;				// Default usage
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;	// Bind as a vertex buffer
	bufferDesc.CPUAccessFlags = 0;						// No CPU access
	bufferDesc.ByteWidth = sizeof(vertices);			// Total size of the buffer
	bufferDesc.StructureByteStride = sizeof(Vertex);	// Size of each vertex

	D3D11_SUBRESOURCE_DATA srData;
	ZeroMemory(&srData, sizeof(D3D11_SUBRESOURCE_DATA));
	srData.pSysMem = vertices;			// Pointer to the vertex data

	pDevice->CreateBuffer(&bufferDesc, &srData, &pVertexBuffer);

	const UINT stride = sizeof(Vertex);	// Size of each vertex
	const UINT offset = 0u;				// No offset
	pContext->IASetVertexBuffers(
		0u,								// Start at slot 0
		1u,								// One buffer
		pVertexBuffer.GetAddressOf(),	// Pointer to the vertex buffer
		&stride,						// Size of each vertex
		&offset							// No offset
	);

	const unsigned short indices[] =
	{
		0,1,2,
		0,2,3,
		0,4,1,
		2,1,5,
	};
	Microsoft::WRL::ComPtr<ID3D11Buffer> pIndexBuffer;
	D3D11_BUFFER_DESC ibd;
	ZeroMemory(&ibd, sizeof(ibd));
	ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	ibd.Usage = D3D11_USAGE_DEFAULT;
	ibd.CPUAccessFlags = 0u;
	ibd.MiscFlags = 0u;
	ibd.ByteWidth = sizeof(indices);
	ibd.StructureByteStride = sizeof(unsigned short);
	D3D11_SUBRESOURCE_DATA isd;
	ZeroMemory(&isd, sizeof(isd));
	isd.pSysMem = indices;
	pDevice->CreateBuffer(&ibd, &isd, &pIndexBuffer);

	pContext->IASetIndexBuffer(pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0u);

	struct ConstantBuffer {
		DirectX::XMMATRIX transform; // Transformation matrix
	};

	const ConstantBuffer cb =
	{
		{
			DirectX::XMMatrixTranspose(
				DirectX::XMMatrixRotationZ(angle) *
				DirectX::XMMatrixScaling(9.0f / 16.0f, 1.0f, 1.0f) // Scale the triangle
			)
		}
	};

	Microsoft::WRL::ComPtr<ID3D11Buffer> pConstantBuffer;
	D3D11_BUFFER_DESC cbd;
	ZeroMemory(&cbd, sizeof(cbd));
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cbd.MiscFlags = 0u;
	cbd.ByteWidth = sizeof(cb);
	cbd.StructureByteStride = 0u;
	D3D11_SUBRESOURCE_DATA csd;
	ZeroMemory(&csd, sizeof(csd));
	csd.pSysMem = &cb;
	pDevice->CreateBuffer(&cbd, &csd, &pConstantBuffer);

	// bind constant buffer to vertex shader
	pContext->VSSetConstantBuffers(0u, 1u, pConstantBuffer.GetAddressOf());


	Microsoft::WRL::ComPtr<ID3DBlob> pBlob;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pPixelShader;
	std::wstring pixelShaderPath = GetExecutableDirectory() + L"\\PixelShader.cso";
	if (!std::filesystem::exists(pixelShaderPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(pixelShaderPath.begin(), pixelShaderPath.end()));
	}
	D3DReadFileToBlob(pixelShaderPath.c_str(), &pBlob);
	pDevice->CreatePixelShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &pPixelShader);

	pContext->PSSetShader(pPixelShader.Get(), nullptr, 0u);		// Set the pixel shader

	Microsoft::WRL::ComPtr<ID3D11VertexShader> pVertexShader;
	std::wstring shaderPath = GetExecutableDirectory() + L"\\VertexShader.cso";
	if (!std::filesystem::exists(shaderPath)) {
		throw std::runtime_error("Shader file not found: " + std::string(shaderPath.begin(), shaderPath.end()));
	}
	D3DReadFileToBlob(shaderPath.c_str(), &pBlob); // Load compiled vertex shader
	pDevice->CreateVertexShader(pBlob->GetBufferPointer(), pBlob->GetBufferSize(), nullptr, &pVertexShader);

	pContext->VSSetShader(pVertexShader.Get(), nullptr, 0u);		// Set the vertex shader

	Microsoft::WRL::ComPtr<ID3D11InputLayout> pInputLayout = nullptr;
	const D3D11_INPUT_ELEMENT_DESC ied[] =
	{
		{ "Position", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};

	pDevice->CreateInputLayout(
		ied,								// Input element description
		(UINT)std::size(ied),				// Number of elements in the array
		pBlob->GetBufferPointer(),			// Pointer to the compiled shader bytecode
		pBlob->GetBufferSize(),				// Size of the compiled shader bytecode
		&pInputLayout						// Output pointer to the input layout
	);

	pContext->IASetInputLayout(pInputLayout.Get());						// Set the input layout

	pContext->OMSetRenderTargets(1u, pTarget.GetAddressOf(), nullptr); // Set the render target

	D3D11_VIEWPORT vp;
	ZeroMemory(&vp, sizeof(D3D11_VIEWPORT));
	vp.Width = 1920;							// Set viewport width
	vp.Height = 1080;							// Set viewport height
	vp.MinDepth = 0;							// Minimum depth
	vp.MaxDepth = 1;							// Maximum depth
	vp.TopLeftX = 0;							// Top-left X position
	vp.TopLeftY = 0;							// Top-left Y position
	pContext->RSSetViewports(1, &vp);			// Set the viewport


	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // Set the primitive topology to triangle list

	pContext->DrawIndexed((UINT)std::size(indices), 0u, 0u); // Draw a triangle (3 vertices, starting at index 0)
}

std::wstring Graphics::GetExecutableDirectory()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);

	std::filesystem::path exePath(path);
	return exePath.parent_path().wstring(); // Strip the exe filename
}