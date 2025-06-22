#include "Graphics.h"

#pragma comment(lib, "d3d11.lib")

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
	ID3D11Resource* pBackBuffer = nullptr;
	if (pSwapChain) {
		HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), reinterpret_cast<void**>(&pBackBuffer));
		if (SUCCEEDED(hr) && pBackBuffer) {
			pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pTarget); // Create a render target view  
			pBackBuffer->Release(); // Release the back buffer after use  
		}
	}
}

Graphics::~Graphics()
{
	// Release the render target view
	if (pTarget != nullptr)
	{
		pTarget->Release();
	}
	// Release the device context
	if (pContext != nullptr) {
		pContext->Release();
	}
	// Release the swap chain
	if (pSwapChain != nullptr) {
		pSwapChain->Release();
	}
	// Release the device
	if (pDevice != nullptr) {
		pDevice->Release();
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
		pContext->ClearRenderTargetView(pTarget, clearColor); // Clear the render target view
	}
}
