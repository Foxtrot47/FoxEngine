#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Logger.h"

namespace SE {

namespace {

// file_time_type::min() for a missing source, so a mesh loaded from its cooked
// entry stays cached until the source appears.
std::filesystem::file_time_type SourceWriteTime(const std::string& path)
{
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    return ec ? (std::filesystem::file_time_type::min)() : writeTime;
}

} // namespace

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

    auto mesh = meshIt->second;

    auto timeIt = m_meshWriteTimes.find(path);
    if (timeIt == m_meshWriteTimes.end() || timeIt->second != SourceWriteTime(path))
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
    m_meshWriteTimes[path] = SourceWriteTime(path);
}

AssetHandle<Texture2D> AssetManager::FindTexture(const std::wstring& path)
{
    auto it = m_textures.find(path);
    return it != m_textures.end() ? it->second : AssetHandle<Texture2D>{};
}

void AssetManager::CacheTexture(const std::wstring& path, const AssetHandle<Texture2D>& texture)
{
    if (texture) m_textures[path] = texture;
}

AssetCacheSnapshot AssetManager::CaptureSnapshot()
{
    AssetCacheSnapshot snapshot;
    std::vector<std::string> meshPaths;
    meshPaths.reserve(m_meshes.size());
    for (const auto& entry : m_meshes) meshPaths.push_back(entry.first);
    for (const auto& path : meshPaths)
        if (auto mesh = FindMesh(path)) snapshot.meshes.emplace(path, std::move(mesh));

    snapshot.textures = m_textures;
    return snapshot;
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
    return static_cast<uint32_t>(m_meshes.size());
}

uint32_t AssetManager::CachedTextureCount() const
{
    return static_cast<uint32_t>(m_textures.size());
}

} // namespace SE
