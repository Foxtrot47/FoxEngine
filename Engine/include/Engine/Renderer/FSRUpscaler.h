#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include <FidelityFX/host/ffx_fsr3upscaler.h>

using Microsoft::WRL::ComPtr;

namespace SE {

// FSR 3.1 upscaler (community DX11 backend). Consumes the render-resolution
// HDR scene, depth, and velocity buffers and produces a display-resolution
// HDR image. Owns the FFX backend interface and the shared intermediate
// textures the 3.1 API requires the app to allocate.
class FSRUpscaler
{
public:
    ~FSRUpscaler() { Shutdown(); }

    bool Init(ID3D11Device* device, uint32_t renderWidth, uint32_t renderHeight,
              uint32_t displayWidth, uint32_t displayHeight);
    void Shutdown();
    bool IsInitialized() const { return m_contextValid; }

    uint32_t RenderWidth() const  { return m_renderWidth; }
    uint32_t RenderHeight() const { return m_renderHeight; }

    // Sub-pixel jitter for the current frame in NDC units (add to the
    // projection matrix as a translation). Call once per frame.
    DirectX::XMFLOAT2 NextJitter();

    struct DispatchParams
    {
        ID3D11Texture2D* color         = nullptr; // render res, HDR
        ID3D11Texture2D* depth         = nullptr; // render res, R32 typeless/float
        ID3D11Texture2D* motionVectors = nullptr; // render res, R16G16_FLOAT, UV delta (curr - prev)
        ID3D11Texture2D* output        = nullptr; // display res, HDR, UAV
        float frameTimeSeconds = 0.0f;
        float cameraNear       = 0.1f;
        float cameraFar        = 1000.0f;
        float cameraFovY       = 1.0f;   // radians
        bool  sharpen          = true;
        float sharpness        = 0.4f;
    };

    void Dispatch(ID3D11DeviceContext* ctx, const DispatchParams& params);

    // Discard accumulated history (scene load, teleport, toggle).
    void ResetHistory() { m_reset = true; }

    // 0.0 improves temporal stability of thin/bright features at the cost of
    // slightly more ghosting in motion; 1.0 is the FSR default.
    void SetVelocityFactor(float factor);

private:
    bool CreateSharedTexture(ID3D11Device* device, const FfxCreateResourceDescription& desc,
                             UINT bindFlags, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& outTex);

    FfxInterface             m_backend = {};
    FfxFsr3UpscalerContext   m_context = {};
    std::vector<uint8_t>     m_scratch;
    bool                     m_contextValid = false;

    ComPtr<ID3D11Texture2D>  m_dilatedDepth;
    ComPtr<ID3D11Texture2D>  m_dilatedMotionVectors;
    ComPtr<ID3D11Texture2D>  m_reconstructedPrevDepth;

    uint32_t m_renderWidth   = 0;
    uint32_t m_renderHeight  = 0;
    uint32_t m_displayWidth  = 0;
    uint32_t m_displayHeight = 0;
    int32_t  m_jitterIndex   = 0;
    DirectX::XMFLOAT2 m_jitterPixels = { 0.0f, 0.0f };
    bool     m_reset = true;
};

} // namespace SE
