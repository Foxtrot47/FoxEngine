#include "EditorThumbnails.h"

#include <algorithm>
#include <DirectXTex.h>

namespace Editor {

namespace {

constexpr uint32_t k_thumbSize = 128;

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

std::string LowerExt(const std::string& path)
{
    const size_t dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return ext;
}

bool LoadImageFile(const std::string& path, DirectX::ScratchImage& loaded)
{
    using namespace DirectX;
    const std::wstring wide = Wide(path);
    const std::string ext = LowerExt(path);
    HRESULT hr;
    if (ext == ".dds")
        hr = LoadFromDDSFile(wide.c_str(), DDS_FLAGS_NONE, nullptr, loaded);
    else if (ext == ".hdr")
        hr = LoadFromHDRFile(wide.c_str(), nullptr, loaded);
    else if (ext == ".tga")
        hr = LoadFromTGAFile(wide.c_str(), nullptr, loaded);
    else
        hr = LoadFromWICFile(wide.c_str(), WIC_FLAGS_NONE, nullptr, loaded);
    return SUCCEEDED(hr) && loaded.GetImageCount() > 0 &&
           loaded.GetMetadata().dimension == TEX_DIMENSION_TEXTURE2D;
}

// Decodes an image file into a <=128px RGBA8 bitmap. For mipped DDS files the
// closest mip is used as the resize source to avoid decompressing 4K images.
bool DecodeThumb(const std::string& path, std::vector<uint8_t>& rgba,
                 uint32_t& width, uint32_t& height)
{
    using namespace DirectX;
    ScratchImage loaded;
    if (!LoadImageFile(path, loaded))
        return false;

    // Smallest mip that is still >= the thumbnail size.
    const TexMetadata& meta = loaded.GetMetadata();
    size_t mip = 0;
    while (mip + 1 < meta.mipLevels &&
           (std::max)(meta.width >> (mip + 1), meta.height >> (mip + 1)) >= k_thumbSize)
        ++mip;
    const Image* source = loaded.GetImage(mip, 0, 0);
    if (!source)
        return false;

    ScratchImage decompressed;
    if (IsCompressed(source->format))
    {
        if (FAILED(Decompress(*source, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed)))
            return false;
        source = decompressed.GetImage(0, 0, 0);
    }

    ScratchImage resized;
    if (source->width > k_thumbSize || source->height > k_thumbSize)
    {
        const float scale = (std::min)((float)k_thumbSize / (float)source->width,
                                       (float)k_thumbSize / (float)source->height);
        const size_t w = (std::max)((size_t)1, (size_t)((float)source->width * scale));
        const size_t h = (std::max)((size_t)1, (size_t)((float)source->height * scale));
        if (FAILED(Resize(*source, w, h, TEX_FILTER_DEFAULT, resized)))
            return false;
        source = resized.GetImage(0, 0, 0);
    }

    ScratchImage converted;
    if (source->format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (FAILED(Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM,
                           TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted)))
            return false;
        source = converted.GetImage(0, 0, 0);
    }

    width = (uint32_t)source->width;
    height = (uint32_t)source->height;
    rgba.resize((size_t)width * height * 4);
    for (uint32_t row = 0; row < height; ++row)
        memcpy(rgba.data() + (size_t)row * width * 4,
               source->pixels + (size_t)row * source->rowPitch,
               (size_t)width * 4);
    return true;
}

} // namespace

ThumbnailCache::~ThumbnailCache()
{
    m_stop.store(true);
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

ID3D11ShaderResourceView* ThumbnailCache::Get(const std::string& path)
{
    const auto texture = m_textures.find(path);
    if (texture != m_textures.end())
        return texture->second ? texture->second->GetSRV() : nullptr;
    if (m_requested.find(path) != m_requested.end())
        return nullptr; // in flight
    m_requested[path] = false;
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(path);
        if (!m_worker.joinable())
            m_worker = std::thread([this] { WorkerLoop(); });
    }
    m_wake.notify_one();
    return nullptr;
}

void ThumbnailCache::WorkerLoop()
{
    for (;;)
    {
        std::string path;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop.load())
                return;
            path = std::move(m_queue.front());
            m_queue.pop_front();
        }
        CpuThumb thumb;
        thumb.path = std::move(path);
        thumb.ok = DecodeThumb(thumb.path, thumb.rgba, thumb.width, thumb.height);
        std::lock_guard lock(m_mutex);
        m_finished.push_back(std::move(thumb));
    }
}

void ThumbnailCache::Update(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    std::vector<CpuThumb> finished;
    {
        std::lock_guard lock(m_mutex);
        finished.swap(m_finished);
    }
    for (auto& thumb : finished)
    {
        std::unique_ptr<SE::Texture2D> texture;
        if (thumb.ok)
        {
            texture = std::make_unique<SE::Texture2D>();
            if (!texture->CreateFromMemory(device, ctx, thumb.rgba.data(),
                                           thumb.width, thumb.height))
                texture.reset();
        }
        m_requested[thumb.path] = true;
        m_textures[thumb.path] = std::move(texture); // nullptr = failed marker
    }
}

} // namespace Editor
