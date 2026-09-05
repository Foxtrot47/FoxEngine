#include "Engine/Scene/MeshAddOperation.h"

#include <chrono>
#include <unordered_set>
#include "Engine/Core/Logger.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/MeshMaterialUtil.h"

namespace SE {
namespace {

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

} // namespace

MeshAddOperation::~MeshAddOperation()
{
    m_cancelled.store(true);
    Join();
}

void MeshAddOperation::Join()
{
    if (m_worker.joinable())
        m_worker.join();
}

bool MeshAddOperation::Start(const std::string& path, AssetCacheSnapshot cache)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_state == MeshAddState::Preparing || m_state == MeshAddState::Uploading)
            return false;
    }
    Join();
    m_cancelled.store(false);
    m_path = LevelDependencyPlan::NormalizePath(path);
    m_error.clear();
    m_cache = std::move(cache);
    m_meshSource = {};
    m_needsMeshUpload = false;
    m_meshUploaded = false;
    m_cpuTextures.clear();
    m_textureUploadIndex = 0;
    m_result.reset();
    m_state = MeshAddState::Preparing;
    m_worker = std::thread([this] { Prepare(); });
    return true;
}

void MeshAddOperation::Prepare()
{
    // Cached mesh: geometry and its textures were decoded during a previous
    // load and are still in the asset cache, so there is no CPU work.
    if (m_cache.meshes.find(m_path) != m_cache.meshes.end())
    {
        std::lock_guard lock(m_mutex);
        m_state = MeshAddState::Uploading;
        return;
    }

    std::string error;
    bool ok = false;
    try
    {
        ok = Mesh::Import(m_path.c_str(), m_meshSource,
                          [this](float) { return !m_cancelled.load(); }, &error);
    }
    catch (const std::exception& exception) { error = exception.what(); }
    if (!ok || m_cancelled.load())
    {
        std::lock_guard lock(m_mutex);
        m_state = MeshAddState::Failed;
        m_error = "Mesh import failed: " + m_path + " (" + error + ")";
        return;
    }
    m_needsMeshUpload = true;

    std::unordered_set<std::string> seen;
    for (const auto& subMesh : m_meshSource.subMeshes)
        for (const auto& raw : { subMesh.material.albedoPath, subMesh.material.normalPath,
                                 subMesh.material.roughnessPath, subMesh.material.emissivePath })
        {
            if (raw.empty()) continue;
            const std::string path = MaterialTexturePath(m_meshSource.directory, raw);
            if (!seen.insert(path).second) continue;
            if (m_cache.textures.find(Wide(path)) != m_cache.textures.end()) continue;
            CpuTexture texture;
            texture.path = path;
            texture.decoded = Texture2D::Decode(Wide(path).c_str(), texture.source);
            if (!texture.decoded)
                SE_LOG_WARN("MeshAdd: texture decode failed: %s", path.c_str());
            m_cpuTextures.push_back(std::move(texture));
            if (m_cancelled.load()) return;
        }

    std::lock_guard lock(m_mutex);
    m_state = MeshAddState::Uploading;
}

void MeshAddOperation::PumpUploads(ID3D11Device* device, AssetManager& assets)
{
    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();
    std::lock_guard lock(m_mutex);
    if (m_state != MeshAddState::Uploading) return;
    while (std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count() < 8.0)
    {
        if (m_needsMeshUpload && !m_meshUploaded)
        {
            auto mesh = assets.FindMesh(m_path);
            if (!mesh)
            {
                mesh = std::make_shared<Mesh>();
                if (!mesh->Upload(device, std::move(m_meshSource)))
                {
                    m_state = MeshAddState::Failed;
                    m_error = "Mesh upload failed: " + m_path;
                    return;
                }
                assets.CacheMesh(m_path, mesh);
            }
            m_meshUploaded = true;
        }
        else if (m_textureUploadIndex < m_cpuTextures.size())
        {
            auto& cpu = m_cpuTextures[m_textureUploadIndex++];
            if (!cpu.decoded) continue;
            if (assets.FindTexture(Wide(cpu.path))) continue;
            auto texture = std::make_shared<Texture2D>();
            if (texture->Upload(device, cpu.source))
                assets.CacheTexture(Wide(cpu.path), texture);
            cpu.source = {};
        }
        else
        {
            auto mesh = assets.FindMesh(m_path);
            if (!mesh)
            {
                m_state = MeshAddState::Failed;
                m_error = "Cached mesh disappeared: " + m_path;
                return;
            }
            m_result = std::make_unique<Result>();
            m_result->path = m_path;
            m_result->mesh = mesh;
            m_result->materials = BuildSubMeshMaterials(assets, *mesh);
            m_cache = {};
            m_cpuTextures.clear();
            m_state = MeshAddState::Complete;
            return;
        }
    }
}

MeshAddState MeshAddOperation::GetState() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

std::string MeshAddOperation::GetError() const
{
    std::lock_guard lock(m_mutex);
    return m_error;
}

std::string MeshAddOperation::GetPath() const
{
    return m_path;
}

std::unique_ptr<MeshAddOperation::Result> MeshAddOperation::TakeResult()
{
    std::lock_guard lock(m_mutex);
    if (m_state != MeshAddState::Complete) return {};
    m_state = MeshAddState::Idle;
    return std::move(m_result);
}

} // namespace SE
