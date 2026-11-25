#include "Engine/Renderer/Texture2D.h"
#include <DirectXTex.h>
#include <windows.h>
#include <wincodec.h>

namespace SE {
namespace {

bool CopyScratch(const DirectX::ScratchImage& image, TextureSourceData& output)
{
    const auto& meta = image.GetMetadata();
    output = {};
    output.width = static_cast<uint32_t>(meta.width);
    output.height = static_cast<uint32_t>(meta.height);
    output.mipLevels = static_cast<uint32_t>(meta.mipLevels);
    output.arraySize = static_cast<uint32_t>(meta.arraySize);
    output.format = meta.format;
    output.cubeMap = meta.IsCubemap();
    output.hasAlpha = DirectX::HasAlpha(meta.format) ||
                      meta.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      meta.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    output.subresources.reserve(image.GetImageCount());
    for (size_t i = 0; i < image.GetImageCount(); ++i)
    {
        const DirectX::Image& src = image.GetImages()[i];
        TextureSubresourceData dst;
        dst.rowPitch = static_cast<uint32_t>(src.rowPitch);
        dst.slicePitch = static_cast<uint32_t>(src.slicePitch);
        dst.bytes.assign(src.pixels, src.pixels + src.slicePitch);
        output.subresources.push_back(std::move(dst));
    }
    return !output.subresources.empty();
}

// Colour formats with both a linear and an sRGB interpretation. The resource is
// created typeless so either view can be made; a typed resource only accepts
// views of its own format.
struct CastableFormat { DXGI_FORMAT typeless, linear, srgb; };
constexpr CastableFormat k_castableFormats[] = {
    { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
    { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB },
    { DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB },
    { DXGI_FORMAT_BC1_TYPELESS,      DXGI_FORMAT_BC1_UNORM,      DXGI_FORMAT_BC1_UNORM_SRGB },
    { DXGI_FORMAT_BC2_TYPELESS,      DXGI_FORMAT_BC2_UNORM,      DXGI_FORMAT_BC2_UNORM_SRGB },
    { DXGI_FORMAT_BC3_TYPELESS,      DXGI_FORMAT_BC3_UNORM,      DXGI_FORMAT_BC3_UNORM_SRGB },
    { DXGI_FORMAT_BC7_TYPELESS,      DXGI_FORMAT_BC7_UNORM,      DXGI_FORMAT_BC7_UNORM_SRGB },
};

const CastableFormat* FindCastableFormat(DXGI_FORMAT format)
{
    for (const CastableFormat& entry : k_castableFormats)
        if (format == entry.linear || format == entry.srgb) return &entry;
    return nullptr;
}

HRESULT CreateSrv(ID3D11Device* device, ID3D11Texture2D* texture,
                  const TextureSourceData& source, DXGI_FORMAT format,
                  ID3D11ShaderResourceView** outView)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = format;
    if (source.cubeMap)
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = UINT(-1);
    }
    else if (source.arraySize > 1)
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = UINT(-1);
        sd.Texture2DArray.ArraySize = source.arraySize;
    }
    else
    {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = UINT(-1);
    }
    return device->CreateShaderResourceView(texture, &sd, outView);
}

HRESULT LoadWICPerThread(const wchar_t* path, DirectX::TexMetadata& meta,
                         DirectX::ScratchImage& image)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    UINT width = 0, height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    hr = image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1);
    if (FAILED(hr)) return hr;
    const DirectX::Image* target = image.GetImage(0, 0, 0);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(target->rowPitch),
                               static_cast<UINT>(target->slicePitch), target->pixels);
    if (SUCCEEDED(hr)) meta = image.GetMetadata();
    return hr;
}

} // namespace

bool Texture2D::Decode(const wchar_t* path, TextureSourceData& output,
                       TextureDecodeMode mode, std::string* error)
{
    using namespace DirectX;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope
    {
        HRESULT result;
        ~ComScope() { if (result == S_OK || result == S_FALSE) CoUninitialize(); }
    } comScope { comResult };
    ScratchImage image;
    TexMetadata meta = {};
    const std::wstring file(path ? path : L"");
    const bool dds = file.size() >= 4 && _wcsicmp(file.c_str() + file.size() - 4, L".dds") == 0;
    HRESULT hr = dds
        ? LoadFromDDSFile(path, DDS_FLAGS_NONE, &meta, image)
        : LoadWICPerThread(path, meta, image);
    if (FAILED(hr))
    {
        if (error) *error = "decode failed (HRESULT " + std::to_string(static_cast<unsigned>(hr)) + ")";
        return false;
    }

    if (mode == TextureDecodeMode::Environment && IsCompressed(meta.format))
    {
        ScratchImage decompressed;
        hr = Decompress(image.GetImages(), image.GetImageCount(), meta,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, decompressed);
        if (FAILED(hr))
        {
            if (error) *error = "environment decompression failed";
            return false;
        }
        image = std::move(decompressed);
    }

    if (image.GetMetadata().mipLevels == 1)
    {
        ScratchImage mipped;
        hr = GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                             TEX_FILTER_LINEAR, 0, mipped);
        if (SUCCEEDED(hr)) image = std::move(mipped);
    }
    return CopyScratch(image, output);
}

bool Texture2D::Upload(ID3D11Device* device, const TextureSourceData& source)
{
    if (!device || source.subresources.empty()) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = source.width;
    desc.Height = source.height;
    desc.MipLevels = source.mipLevels;
    desc.ArraySize = source.arraySize;
    const CastableFormat* castable = FindCastableFormat(source.format);
    desc.Format = castable ? castable->typeless : source.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = source.cubeMap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    std::vector<D3D11_SUBRESOURCE_DATA> initial(source.subresources.size());
    for (size_t i = 0; i < source.subresources.size(); ++i)
    {
        initial[i].pSysMem = source.subresources[i].bytes.data();
        initial[i].SysMemPitch = source.subresources[i].rowPitch;
        initial[i].SysMemSlicePitch = source.subresources[i].slicePitch;
    }
    if (FAILED(device->CreateTexture2D(&desc, initial.data(), &m_texture))) return false;
    m_srv.Reset();
    m_srvSrgb.Reset();
    if (castable)
    {
        if (FAILED(CreateSrv(device, m_texture.Get(), source, castable->linear, &m_srv))) return false;
        if (FAILED(CreateSrv(device, m_texture.Get(), source, castable->srgb, &m_srvSrgb))) return false;
    }
    else
    {
        // No sRGB variant (float or two-channel data): both bindings read it as stored.
        if (FAILED(device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv))) return false;
        m_srvSrgb = m_srv;
    }

    m_width = source.width;
    m_height = source.height;
    m_hasAlpha = source.hasAlpha;
    return true;
}

bool Texture2D::CreateFromMemory(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                 const uint8_t* rgba, uint32_t width, uint32_t height)
{
    return CreateSRV(device, ctx, rgba, width, height);
}

void Texture2D::BindPS(ID3D11DeviceContext* ctx, uint32_t slot) const
{
    ctx->PSSetShaderResources(slot, 1, m_srv.GetAddressOf());
}

void Texture2D::BindPSSrgb(ID3D11DeviceContext* ctx, uint32_t slot) const
{
    ctx->PSSetShaderResources(slot, 1, m_srvSrgb.GetAddressOf());
}

bool Texture2D::CreateSRV(ID3D11Device* device, ID3D11DeviceContext* ctx,
                          const uint8_t* rgba, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_texture))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = UINT(-1);
    if (FAILED(device->CreateShaderResourceView(m_texture.Get(), &sd, &m_srv))) return false;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    m_srvSrgb.Reset();
    if (FAILED(device->CreateShaderResourceView(m_texture.Get(), &sd, &m_srvSrgb))) return false;

    ctx->UpdateSubresource(m_texture.Get(), 0, nullptr, rgba, width * 4, 0);
    ctx->GenerateMips(m_srv.Get());
    m_hasAlpha = true;
    return true;
}

} // namespace SE
