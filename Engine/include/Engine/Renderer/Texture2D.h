#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace SE {

enum class TextureDecodeMode { Color, Environment };

struct TextureSubresourceData
{
    std::vector<uint8_t> bytes;
    uint32_t rowPitch = 0;
    uint32_t slicePitch = 0;
};

struct TextureSourceData
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    uint32_t arraySize = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool cubeMap = false;
    bool hasAlpha = false;
    std::vector<TextureSubresourceData> subresources;
};

class Texture2D
{
public:
    static bool Decode(const wchar_t* path, TextureSourceData& output,
                       TextureDecodeMode mode = TextureDecodeMode::Color,
                       std::string* error = nullptr);
    bool Upload(ID3D11Device* device, const TextureSourceData& source);
    bool CreateFromMemory(ID3D11Device* device, ID3D11DeviceContext* ctx,
                          const uint8_t* rgba, uint32_t width, uint32_t height);
    void BindPS(ID3D11DeviceContext* ctx, uint32_t slot) const;

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsValid() const { return m_srv != nullptr; }
    bool HasAlpha() const { return m_hasAlpha; }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }

private:
    bool CreateSRV(ID3D11Device* device, ID3D11DeviceContext* ctx,
                   const uint8_t* rgba, uint32_t width, uint32_t height);

    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_hasAlpha = false;
};

} // namespace SE
