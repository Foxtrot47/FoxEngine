#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include "Engine/Renderer/ConstantBuffer.h"
#include "Engine/Renderer/FullscreenQuad.h"
#include "Engine/Renderer/ShaderLibrary.h"

namespace SE {

class EnvironmentLighting
{
public:
    bool Init(ID3D11Device* device, ShaderLibrary& shaders);
    bool Build(ID3D11Device* device, ID3D11DeviceContext* ctx,
               ID3D11ShaderResourceView* panorama);
    void Clear();

    ID3D11ShaderResourceView* GetIrradianceSRV() const { return m_irradianceSRV.Get(); }
    ID3D11ShaderResourceView* GetSpecularSRV() const { return m_specularSRV.Get(); }
    ID3D11ShaderResourceView* GetBRDFLutSRV() const { return m_brdfLutSRV.Get(); }

private:
    struct FilterCB
    {
        float roughness = 0.0f;
        int mode = 0;
        int sampleCount = 64;
        float sourceMipCount = 1.0f;
    };

    bool CreateFilteredTarget(ID3D11Device* device, uint32_t width, uint32_t height,
                              uint32_t mipLevels,
                              Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
                              std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>& rtvs);
    bool CreateBRDFLut(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void DrawFilter(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                    uint32_t width, uint32_t height,
                    ID3D11ShaderResourceView* source, const FilterCB& data);

    const ShaderPermutation* m_filterPerm = nullptr;
    FullscreenQuad m_quad;
    ConstantBuffer<FilterCB> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_irradianceTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_irradianceSRV;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_irradianceRTVs;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_specularTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_specularSRV;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_specularRTVs;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_brdfLutTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_brdfLutSRV;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_brdfLutRTV;
};

} // namespace SE
