#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Logger.h"

namespace SE {

void AssetManager::Init(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    m_device  = device;
    m_context = ctx;
    SE_LOG_INFO("AssetManager initialised");
}

AssetHandle<Mesh> AssetManager::FindMesh(const std::string& path)
{
    auto meshIt = m_meshes.find(path);
    if (meshIt == m_meshes.end())
        return {};

    auto mesh = meshIt->second.lock();
    if (!mesh)
    {
        m_meshes.erase(meshIt);
        m_meshWriteTimes.erase(path);
        return {};
    }

    std::error_code ec;
    const auto currentWriteTime = std::filesystem::last_write_time(path, ec);
    auto timeIt = m_meshWriteTimes.find(path);
    if (ec || timeIt == m_meshWriteTimes.end() || timeIt->second != currentWriteTime)
    {
        SE_LOG_INFO("AssetManager: source mesh changed; invalidating '%s'", path.c_str());
        m_meshes.erase(meshIt);
        m_meshWriteTimes.erase(path);
        return {};
    }

    return mesh;
}

void AssetManager::CacheMesh(const std::string& path, const AssetHandle<Mesh>& mesh)
{
    if (!mesh)
        return;

    m_meshes[path] = mesh;
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (!ec)
        m_meshWriteTimes[path] = writeTime;
    else
        m_meshWriteTimes.erase(path);
}

AssetHandle<Texture2D> AssetManager::FindTexture(const std::wstring& path)
{
    auto it = m_textures.find(path);
    if (it != m_textures.end())
        if (auto h = it->second.lock()) return h;
    if (it != m_textures.end()) m_textures.erase(it);
    return {};
}

void AssetManager::CacheTexture(const std::wstring& path, const AssetHandle<Texture2D>& texture)
{
    if (texture) m_textures[path] = texture;
}

AssetHandle<Texture2D> AssetManager::GetDefaultWhite()
{
    if (auto h = m_defaultWhite.lock()) return h;
    auto tex = std::make_shared<Texture2D>();
    uint8_t px[4] = { 255, 255, 255, 255 };
    tex->CreateFromMemory(m_device, m_context, px, 1, 1);
    m_defaultWhite = tex;
    return tex;
}

AssetHandle<Texture2D> AssetManager::GetDefaultBlack()
{
    if (auto h = m_defaultBlack.lock()) return h;
    auto tex = std::make_shared<Texture2D>();
    uint8_t px[4] = { 0, 0, 0, 255 };
    tex->CreateFromMemory(m_device, m_context, px, 1, 1);
    m_defaultBlack = tex;
    return tex;
}

AssetHandle<Texture2D> AssetManager::GetDefaultNormal()
{
    if (auto h = m_defaultNormal.lock()) return h;
    auto tex = std::make_shared<Texture2D>();
    uint8_t px[4] = { 128, 128, 255, 255 };
    tex->CreateFromMemory(m_device, m_context, px, 1, 1);
    m_defaultNormal = tex;
    return tex;
}

uint32_t AssetManager::CachedMeshCount() const
{
    uint32_t n = 0;
    for (auto& [k, w] : m_meshes)
        if (!w.expired()) ++n;
    return n;
}

uint32_t AssetManager::CachedTextureCount() const
{
    uint32_t n = 0;
    for (auto& [k, w] : m_textures)
        if (!w.expired()) ++n;
    return n;
}

} // namespace SE
