#include "Engine/Renderer/FSRUpscaler.h"
#include "Engine/Core/Logger.h"

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

namespace SE {

namespace {

void FfxMessageCallback(FfxMsgType type, const wchar_t* message)
{
    char buf[1024];
    size_t converted = 0;
    wcstombs_s(&converted, buf, message, _TRUNCATE);
    if (type == FFX_MESSAGE_TYPE_ERROR)
        SE_LOG_ERROR("[FSR] %s", buf);
    else
        SE_LOG_WARN("[FSR] %s", buf);
}

FfxResource WrapResource(ID3D11Resource* resource, const wchar_t* name,
                         FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ)
{
    if (!resource)
        return {};
    return ffxGetResourceDX11_Fsr31(resource, GetFfxResourceDescriptionDX11(resource), name, state);
}

} // namespace

bool FSRUpscaler::Init(ID3D11Device* device, uint32_t renderWidth, uint32_t renderHeight,
                       uint32_t displayWidth, uint32_t displayHeight)
{
    Shutdown();

    m_renderWidth   = renderWidth;
    m_renderHeight  = renderHeight;
    m_displayWidth  = displayWidth;
    m_displayHeight = displayHeight;

    const size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
    m_scratch.resize(scratchSize);

    FfxErrorCode err = ffxGetInterfaceDX11(&m_backend, ffxGetDeviceDX11_Fsr31(device),
                                           m_scratch.data(), scratchSize,
                                           FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if (err != FFX_OK)
    {
        SE_LOG_ERROR("FSR: ffxGetInterfaceDX11 failed (%d)", static_cast<int>(err));
        return false;
    }

    FfxFsr3UpscalerContextDescription desc = {};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                 FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
#if defined(SE_DEBUG)
    desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
#endif
    desc.maxRenderSize  = { renderWidth, renderHeight };
    desc.maxUpscaleSize = { displayWidth, displayHeight };
    desc.fpMessage      = FfxMessageCallback;
    desc.backendInterface = m_backend;

    err = ffxFsr3UpscalerContextCreate(&m_context, &desc);
    if (err != FFX_OK)
    {
        SE_LOG_ERROR("FSR: context create failed (%d)", static_cast<int>(err));
        return false;
    }
    m_contextValid = true;

    // FSR 3.1 requires the app to allocate three shared intermediate textures.
    FfxFsr3UpscalerSharedResourceDescriptions shared = {};
    ffxFsr3UpscalerGetSharedResourceDescriptions(&m_context, &shared);

    bool ok = true;
    ok &= CreateSharedTexture(device, shared.dilatedDepth,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              DXGI_FORMAT_R32_FLOAT, m_dilatedDepth);
    ok &= CreateSharedTexture(device, shared.dilatedMotionVectors,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              DXGI_FORMAT_R16G16_FLOAT, m_dilatedMotionVectors);
    ok &= CreateSharedTexture(device, shared.reconstructedPrevNearestDepth,
                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              DXGI_FORMAT_R32_UINT, m_reconstructedPrevDepth);
    if (!ok)
    {
        Shutdown();
        return false;
    }

    m_jitterIndex = 0;
    m_reset       = true;
    SE_LOG_INFO("FSR 3.1 upscaler initialized: %ux%u -> %ux%u",
                renderWidth, renderHeight, displayWidth, displayHeight);
    return true;
}

bool FSRUpscaler::CreateSharedTexture(ID3D11Device* device, const FfxCreateResourceDescription& desc,
                                      UINT bindFlags, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& outTex)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = desc.resourceDescription.width;
    td.Height           = desc.resourceDescription.height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = format;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = bindFlags;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, outTex.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SE_LOG_ERROR("FSR: failed to create shared texture (%ux%u)", td.Width, td.Height);
        return false;
    }
    return true;
}

void FSRUpscaler::Shutdown()
{
    if (m_contextValid)
    {
        ffxFsr3UpscalerContextDestroy(&m_context);
        m_contextValid = false;
    }
    m_dilatedDepth.Reset();
    m_dilatedMotionVectors.Reset();
    m_reconstructedPrevDepth.Reset();
    m_scratch.clear();
    m_backend = {};
}

void FSRUpscaler::SetVelocityFactor(float factor)
{
    if (!m_contextValid)
        return;
    ffxFsr3UpscalerSetConstant(&m_context,
        FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, &factor);
}

DirectX::XMFLOAT2 FSRUpscaler::NextJitter()
{
    const int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(
        static_cast<int32_t>(m_renderWidth), static_cast<int32_t>(m_displayWidth));

    float jx = 0.0f, jy = 0.0f;
    ffxFsr3UpscalerGetJitterOffset(&jx, &jy, m_jitterIndex, phaseCount);
    m_jitterIndex = (m_jitterIndex + 1) % phaseCount;
    m_jitterPixels = { jx, jy };

    // Pixel-space jitter -> NDC translation for the projection matrix.
    return { 2.0f * jx / static_cast<float>(m_renderWidth),
             -2.0f * jy / static_cast<float>(m_renderHeight) };
}

void FSRUpscaler::Dispatch(ID3D11DeviceContext* ctx, const DispatchParams& params)
{
    if (!m_contextValid)
        return;

    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList   = ffxGetCommandListDX11(ctx);
    d.color         = WrapResource(params.color, L"FSR_Color");
    d.depth         = WrapResource(params.depth, L"FSR_Depth");
    d.motionVectors = WrapResource(params.motionVectors, L"FSR_Velocity");
    d.exposure      = {};
    d.reactive      = {};
    d.transparencyAndComposition = {};
    d.dilatedDepth = WrapResource(m_dilatedDepth.Get(), L"FSR_DilatedDepth",
                                  FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.dilatedMotionVectors = WrapResource(m_dilatedMotionVectors.Get(), L"FSR_DilatedMVs",
                                          FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.reconstructedPrevNearestDepth = WrapResource(m_reconstructedPrevDepth.Get(), L"FSR_ReconPrevDepth",
                                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.output = WrapResource(params.output, L"FSR_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    d.jitterOffset = { m_jitterPixels.x, m_jitterPixels.y };
    // Velocity buffer stores (currUV - prevUV); FSR wants pixels pointing
    // from current to previous, so scale by the negated render size.
    d.motionVectorScale = { -static_cast<float>(m_renderWidth),
                            -static_cast<float>(m_renderHeight) };
    d.renderSize  = { m_renderWidth, m_renderHeight };
    d.upscaleSize = { m_displayWidth, m_displayHeight };

    d.enableSharpening = params.sharpen;
    d.sharpness        = params.sharpness;
    d.frameTimeDelta   = params.frameTimeSeconds * 1000.0f;
    d.preExposure      = 1.0f;
    d.reset            = m_reset;
    d.cameraNear       = params.cameraNear;
    d.cameraFar        = params.cameraFar;
    d.cameraFovAngleVertical = params.cameraFovY;
    d.viewSpaceToMetersFactor = 1.0f;

    const FfxErrorCode err = ffxFsr3UpscalerContextDispatch(&m_context, &d);
    if (err != FFX_OK)
        SE_LOG_ERROR("FSR: dispatch failed (%d)", static_cast<int>(err));

    m_reset = false;
}

} // namespace SE
