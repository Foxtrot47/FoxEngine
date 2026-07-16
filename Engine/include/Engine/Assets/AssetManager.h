#pragma once
#include <d3d11.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include "Engine/Renderer/Mesh.h"
#include "Engine/Renderer/Texture2D.h"

namespace SE {

template<typename T>
using AssetHandle = std::shared_ptr<T>;

struct AssetCacheSnapshot
{
    std::unordered_map<std::string, AssetHandle<Mesh>> meshes;
    std::unordered_map<std::wstring, AssetHandle<Texture2D>> textures;
};

class AssetManager
{
public:
    void Init(ID3D11Device* device, ID3D11DeviceContext* ctx);

    AssetHandle<Mesh>      FindMesh  (const std::string&  path);
    AssetHandle<Texture2D> FindTexture(const std::wstring& path);
    void CacheMesh(const std::string& path, const AssetHandle<Mesh>& mesh);
    void CacheTexture(const std::wstring& path, const AssetHandle<Texture2D>& texture);
    AssetCacheSnapshot CaptureSnapshot();

    // Fallback 1×1 textures for submeshes missing a particular map.
    AssetHandle<Texture2D> GetDefaultWhite();    // flat white albedo / roughness (all channels = 1.0)
    AssetHandle<Texture2D> GetDefaultBlack();    // flat black metallic  (all channels = 0.0)
    AssetHandle<Texture2D> GetDefaultNormal();   // flat tangent-space normal (128,128,255)

    uint32_t CachedMeshCount()    const;
    uint32_t CachedTextureCount() const;

private:
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    std::unordered_map<std::string, AssetHandle<Mesh>> m_meshes;
    std::unordered_map<std::string, std::filesystem::file_time_type> m_meshWriteTimes;
    std::unordered_map<std::wstring, AssetHandle<Texture2D>> m_textures;
    std::weak_ptr<Texture2D> m_defaultWhite;
    std::weak_ptr<Texture2D> m_defaultBlack;
    std::weak_ptr<Texture2D> m_defaultNormal;
};

} // namespace SE
