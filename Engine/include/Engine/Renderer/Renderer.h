#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include "Engine/Renderer/RenderStateCache.h"

using Microsoft::WRL::ComPtr;

namespace SE {

class Renderer
{
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    // Clears and binds the swap-chain back buffer. Call at the start of each frame.
    void BeginFrame(float r, float g, float b, float a = 1.0f);

    // Bind the swap chain back-buffer RTV (no depth). Used for the final composite.
    void BindBackBuffer(ID3D11DeviceContext* ctx);

    // Just present the swap chain. Call after post-process + ImGui have drawn to back buffer.
    void Present();

    // Rebuild swap chain buffers and MSAA surfaces after a window resize.
    void Resize(uint32_t width, uint32_t height);

    ID3D11Device*        GetDevice()  const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    RenderStateCache&    GetStateCache()    { return m_stateCache; }

private:
    void CreateSurfaces();
    void ReleaseSurfaces();

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain>         m_swapChain;
    ComPtr<ID3D11Texture2D>        m_backBuffer;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    RenderStateCache m_stateCache;
};

} // namespace SE
