#include "Graphics.h"
#include "LightManager.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCompiler.lib")

Graphics::Graphics(HWND hWnd, int windowWidth, int windowHeight) : pDevice(nullptr), pSwapChain(nullptr), pContext(nullptr)
{
	HRESULT hr = E_FAIL;

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferCount = 1;                                   // One back buffer
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;    // Use 32-bit color
	scd.BufferDesc.Width = windowWidth;                    // Set the back buffer width
	scd.BufferDesc.Height = windowHeight;                  // Set the back buffer height
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;     // How the swap chain is to be used
	scd.OutputWindow = hWnd;                               // The window to be used
	scd.SampleDesc.Count = 1;                              // No multisampling
	scd.Windowed = TRUE;                                   // Windowed/full-screen mode
	scd.Flags = 0;                                         // No special flags
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;             // Discard the old back buffer when presenting

	// Create a device, device context and swap chain using the information in the scd struct
	hr = D3D11CreateDeviceAndSwapChain(
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
	if (FAILED(hr)) {
		throw new std::runtime_error("Failed to create device and swap chain");
	}

	pLightManager = std::make_shared<LightManager>(*this);

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

void Graphics::BeginFrame()
{
	if (imGuiEnabled)
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
	}
}

void Graphics::BeginRenderPass()
{
	pLightManager->Update(*this);
	pLightManager->Bind(*this);
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

void Graphics::SetCamera(DirectX::FXMMATRIX _camera)
{
	camera = _camera;
}

DirectX::XMMATRIX Graphics::GetCamera() const
{
	return camera;
}
