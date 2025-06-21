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
		nullptr,                       // Default adapter
		D3D_DRIVER_TYPE_HARDWARE,   // Use hardware rendering
		nullptr,                       // No software device
		0,                          // No flags
		nullptr,                       // Default feature level array
		0,                          // Default feature level array size
		D3D11_SDK_VERSION,          // Always set this to D3D11_SDK_VERSION
		&scd,                       // The swap chain description
		&pSwapChain,                // The swap chain
		&pDevice,                   // The device
		nullptr,                       // The feature level
		&pContext                   // The device context
	);
}

Graphics::~Graphics()
{
	// Release the device context
	if (pContext) {
		pContext->Release();
	}
	// Release the swap chain
	if (pSwapChain) {
		pSwapChain->Release();
	}
	// Release the device
	if (pDevice) {
		pDevice->Release();
	}
}
