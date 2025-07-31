#include "Graphics.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

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
	scd.SampleDesc.Count = 1;                              // No multisampling
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

	D3D11_DEPTH_STENCIL_DESC descDepthStencil;
	ZeroMemory(&descDepthStencil, sizeof(D3D11_DEPTH_STENCIL_DESC));
	descDepthStencil.DepthEnable = TRUE;
	descDepthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	descDepthStencil.DepthFunc = D3D11_COMPARISON_LESS;

	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> pDepthStencilState;
	HRESULT hr = pDevice->CreateDepthStencilState(&descDepthStencil, &pDepthStencilState);
	pContext->OMSetDepthStencilState(pDepthStencilState.Get(), 1u); // Set the depth stencil state

	// create depth stencil texture
	Microsoft::WRL::ComPtr<ID3D11Texture2D> pDepthStencilTexture;
	D3D11_TEXTURE2D_DESC descDepth;
	ZeroMemory(&descDepth, sizeof(D3D11_TEXTURE2D_DESC));
	descDepth.Width = 1920; // Set the width of the depth stencil buffer
	descDepth.Height = 1080; // Set the height of the depth stencil buffer
	descDepth.MipLevels = 1u; // No mipmaps
	descDepth.ArraySize = 1u; // Single texture
	descDepth.Format = DXGI_FORMAT_D32_FLOAT; // 32-bit depth buffer
	descDepth.SampleDesc.Count = 1; // No multisampling
	descDepth.SampleDesc.Quality = 0u; // No multisampling quality
	descDepth.Usage = D3D11_USAGE_DEFAULT;
	descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL; // Bind as a depth stencil buffer
	pDevice->CreateTexture2D(&descDepth, nullptr, &pDepthStencilTexture);	// Create the depth stencil buffer

	// create view of the depth stencil texture
	D3D11_DEPTH_STENCIL_VIEW_DESC descDepthStencilView;
	ZeroMemory(&descDepthStencilView, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));
	descDepthStencilView.Format = DXGI_FORMAT_D32_FLOAT;
	descDepthStencilView.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	descDepthStencilView.Texture2D.MipSlice = 0; // Use the first mip level
	pDevice->CreateDepthStencilView(
		pDepthStencilTexture.Get(), 
		&descDepthStencilView,
		&pDepthStencilView
	);

	pContext->OMSetRenderTargets(1u, pTarget.GetAddressOf(), pDepthStencilView.Get()); // Set the render target and depth stencil view

	// configure viewport
	D3D11_VIEWPORT vp = {};
	vp.Width = 1920;							// Set viewport width
	vp.Height = 1080;							// Set viewport height
	vp.MinDepth = 0;							// Minimum depth
	vp.MaxDepth = 1;							// Maximum depth
	vp.TopLeftX = 0;							// Top-left X position
	vp.TopLeftY = 0;							// Top-left Y position
	pContext->RSSetViewports(1, &vp);

	ImGui_ImplDX11_Init(pDevice.Get(), pContext.Get());
}

Graphics::~Graphics()
{
	ImGui_ImplDX11_Shutdown();
}

void Graphics::EndFrame()
{
	if (imGuiEnabled)
	{
		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}
	// Present the back buffer to the front buffer
	if (pSwapChain) {
		pSwapChain->Present(1, 0); // 1 for vsync, 0 for no flags
	}
}

void Graphics::BeginFrame(float red, float green, float blue)
{
	if (imGuiEnabled)
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	}

	// Clear the back buffer to a specific color
	if (pContext) {
		const float clearColor[4] = { red, green, blue, 1.0f }; // RGBA color
		pContext->ClearRenderTargetView(pTarget.Get(), clearColor); // Clear the render target view
		pContext->ClearDepthStencilView(
			pDepthStencilView.Get(),
			D3D11_CLEAR_DEPTH, // Clear depth and stencil
			1.0f, // Depth value to clear to
			0u // Stencil value to clear to
		); // Clear the depth stencil view
	}
}

void Graphics::DrawIndexed(UINT count)
{
	pContext->DrawIndexed(count, 0u, 0u);
}

void Graphics::SetProjection(DirectX::FXMMATRIX proj)
{
	projection = proj; 
}

DirectX::XMMATRIX Graphics::GetProjection() const
{
	return projection;
}

void Graphics::EnableImGui()
{
	imGuiEnabled = true;
}

void Graphics::DisableImGui()
{
	imGuiEnabled = false;
}

bool Graphics::IsImGuiEnabled() const
{
	return imGuiEnabled;
}
