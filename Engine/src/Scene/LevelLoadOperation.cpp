#include "Engine/Scene/LevelLoadOperation.h"
#include "Engine/Scene/LevelAssetLoader.h"
#include "Engine/Scene/SceneLoader.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace SE {
namespace {

std::wstring Wide(const std::string& path) { return { path.begin(), path.end() }; }

std::string StemDDS(const std::string& raw)
{
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    const size_t slash = p.rfind('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    const size_t dot = name.rfind('.');
    return (dot == std::string::npos ? name : name.substr(0, dot)) + ".dds";
}

std::string MaterialTexturePath(const std::string& directory, const std::string& raw)
{
    const std::string dds = StemDDS(raw);
    const std::string primary = LevelDependencyPlan::NormalizePath(directory + "Textures/" + dds);
    const std::string fallback = LevelDependencyPlan::NormalizePath(directory + dds);
    std::error_code error;
    return std::filesystem::exists(primary, error) ? primary : fallback;
}

AssetHandle<Texture2D> FindTexture(AssetManager& assets, const std::string& path)
{
    const std::string normalized = LevelDependencyPlan::NormalizePath(path);
    return normalized.empty() ? AssetHandle<Texture2D>{} : assets.FindTexture(Wide(normalized));
}

ForwardPipeline::SubMat ResolveObjectMaterial(AssetManager& assets,
                                              const SceneDescriptor::SceneObject& object)
{
    ForwardPipeline::SubMat mat;
    mat.albedo = FindTexture(assets, object.albedoPath);
    mat.normal = FindTexture(assets, object.normalPath);
    mat.roughness = FindTexture(assets, object.roughnessPath);
    mat.metallic = FindTexture(assets, object.metallicPath);
    mat.emissive = FindTexture(assets, object.emissivePath);
    if (!mat.albedo) mat.albedo = assets.GetDefaultWhite();
    if (!mat.normal) mat.normal = assets.GetDefaultNormal();
    if (!mat.roughness) mat.roughness = assets.GetDefaultWhite();
    if (!mat.metallic) mat.metallic = assets.GetDefaultBlack();
    if (!mat.emissive) mat.emissive = assets.GetDefaultBlack();
    return mat;
}

} // namespace

LevelLoadOperation::~LevelLoadOperation() { Cancel(); }

void LevelLoadOperation::Join()
{
    if (m_coordinator.joinable()) m_coordinator.join();
}

bool LevelLoadOperation::Start(const std::string& path, AssetCacheSnapshot cache)
{
    Cancel();
    {
        std::lock_guard lock(m_mutex);
        m_status = {};
        m_status.state = LevelLoadState::Preparing;
        m_prepared.reset();
        m_cpuMeshes.clear();
        m_cpuTextures.clear();
        m_cache = std::move(cache);
        m_readyMeshes = m_cache.meshes;
        m_readyTextures = m_cache.textures;
        m_meshUploadIndex = 0;
        m_textureUploadIndex = 0;
        m_uploadMilliseconds = 0.0;
    }
    m_cancelled.store(false);
    m_coordinator = std::thread([this, path] {
        try { Prepare(path); }
        catch (const std::exception& exception) { SetTerminal(LevelLoadState::Failed, exception.what()); }
        catch (...) { SetTerminal(LevelLoadState::Failed, "Unknown CPU preparation failure"); }
    });
    return true;
}

void LevelLoadOperation::Cancel()
{
    m_cancelled.store(true);
    Join();
    std::lock_guard lock(m_mutex);
    if (m_status.state == LevelLoadState::Preparing || m_status.state == LevelLoadState::Uploading)
        m_status.state = LevelLoadState::Cancelled;
}

void LevelLoadOperation::SetTerminal(LevelLoadState state, const std::string& reason)
{
    std::lock_guard lock(m_mutex);
    m_status.state = state;
    m_status.failureReason = reason;
}

void LevelLoadOperation::PublishProgress(float value, const std::string& current)
{
    std::lock_guard lock(m_mutex);
    m_status.progress = (std::max)(m_status.progress, (std::min)(value, 1.0f));
    if (!current.empty()) m_status.currentAsset = current;
}

void LevelLoadOperation::AddTiming(const char* name, std::chrono::steady_clock::duration elapsed)
{
    std::lock_guard lock(m_mutex);
    m_status.timings.push_back({ name, std::chrono::duration<double, std::milli>(elapsed).count() });
}

void LevelLoadOperation::Prepare(std::string path)
{
    using Clock = std::chrono::steady_clock;
    const auto parseStart = Clock::now();
    const auto result = LevelAssetLoader::LoadFromFile(path);
    if (m_cancelled.load()) return;
    if (!result.Succeeded()) { SetTerminal(LevelLoadState::Failed, "Level parsing failed"); return; }

    auto prepared = std::make_unique<PreparedLevel>();
    prepared->level = result.asset;
    prepared->diagnostics = result.diagnostics;
    prepared->plan = LevelDependencyPlan::Build(prepared->level);
    if (!SceneLoader::BuildDescriptor(prepared->level, prepared->descriptor))
    {
        SetTerminal(LevelLoadState::Failed, "Could not build runtime level descriptor");
        return;
    }
    AddTiming("Parse level", Clock::now() - parseStart);
    {
        std::lock_guard lock(m_mutex);
        m_status.dependencyCount = static_cast<uint32_t>(prepared->plan.dependencies.size());
    }
    PublishProgress(0.05f, path);

    std::vector<LevelDependency> meshDependencies;
    for (const auto& dependency : prepared->plan.dependencies)
        if (dependency.kind == LevelDependencyKind::Mesh) meshDependencies.push_back(dependency);
    std::vector<LevelDependency> meshes;
    for (const auto& dependency : meshDependencies)
    {
        if (m_cache.meshes.find(dependency.path) == m_cache.meshes.end())
            meshes.push_back(dependency);
        else
        {
            std::lock_guard lock(m_mutex);
            ++m_status.cacheHits;
            ++m_status.completedDependencies;
        }
    }
    std::vector<CpuMesh> cpuMeshes(meshes.size());
    std::atomic<size_t> meshCursor { 0 };
    std::atomic<bool> meshFailed { false };
    std::string meshFailure;
    std::mutex failureMutex;
    const auto meshStart = Clock::now();
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned available = hardware > 1 ? hardware - 1 : 1;
    const unsigned meshWorkers = (std::min)(available, static_cast<unsigned>(meshes.size()));
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < meshWorkers; ++w)
    {
        workers.emplace_back([&] {
            for (;;)
            {
                const size_t index = meshCursor.fetch_add(1);
                if (index >= meshes.size() || m_cancelled.load()) return;
                CpuMesh item;
                item.path = meshes[index].path;
                std::string error;
                bool ok = false;
                try
                {
                    ok = Mesh::Import(item.path.c_str(), item.source,
                        [this](float) { return !m_cancelled.load(); }, &error);
                }
                catch (const std::exception& exception) { error = exception.what(); }
                if (!ok && !m_cancelled.load())
                {
                    meshFailed.store(true);
                    std::lock_guard failureLock(failureMutex);
                    if (meshFailure.empty()) meshFailure = "Required mesh failed: " + item.path + " (" + error + ")";
                }
                cpuMeshes[index] = std::move(item);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    AddTiming("Import geometry", Clock::now() - meshStart);
    PublishProgress(0.50f, meshes.empty() ? path : meshes.back().path);
    if (m_cancelled.load()) return;
    if (meshFailed.load())
    {
        std::lock_guard lock(m_mutex);
        ++m_status.requiredFailures;
        m_status.state = LevelLoadState::Failed;
        m_status.failureReason = meshFailure;
        return;
    }

    std::unordered_set<std::string> texturePaths;
    std::vector<LevelDependency> textures;
    auto addTexture = [&](LevelDependency dependency, bool dynamic) {
        dependency.path = LevelDependencyPlan::NormalizePath(dependency.path);
        if (!dependency.path.empty() && texturePaths.insert(dependency.path).second)
        {
            textures.push_back(dependency);
            if (dynamic) prepared->plan.dependencies.push_back(dependency);
        }
    };
    const auto plannedDependencies = prepared->plan.dependencies;
    for (const auto& dependency : plannedDependencies)
        if (dependency.kind != LevelDependencyKind::Mesh) addTexture(dependency, false);
    for (const auto& mesh : cpuMeshes)
        for (const auto& subMesh : mesh.source.subMeshes)
            for (const auto& raw : { subMesh.material.albedoPath, subMesh.material.normalPath,
                                     subMesh.material.roughnessPath, subMesh.material.emissivePath })
                if (!raw.empty()) addTexture({ MaterialTexturePath(mesh.source.directory, raw), mesh.path,
                                               LevelDependencyKind::Texture, false }, true);
    for (const auto& dependency : meshDependencies)
    {
        const auto cached = m_cache.meshes.find(dependency.path);
        if (cached == m_cache.meshes.end()) continue;
        const auto& mesh = *cached->second;
        for (uint32_t i = 0; i < mesh.GetSubMeshCount(); ++i)
        {
            const auto subMesh = mesh.GetSubMeshInfo(i);
            for (const auto& raw : { subMesh.albedoPath, subMesh.normalPath,
                                     subMesh.roughnessPath, subMesh.emissivePath })
                if (!raw.empty()) addTexture({ MaterialTexturePath(mesh.GetDirectory(), raw),
                                               dependency.path, LevelDependencyKind::Texture, false }, true);
        }
    }

    std::vector<LevelDependency> textureTasks;
    textureTasks.reserve(textures.size());
    for (const auto& dependency : textures)
    {
        if (m_cache.textures.find(Wide(dependency.path)) == m_cache.textures.end())
            textureTasks.push_back(dependency);
        else
        {
            std::lock_guard lock(m_mutex);
            ++m_status.cacheHits;
            ++m_status.completedDependencies;
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_status.dependencyCount = static_cast<uint32_t>(meshDependencies.size() + textures.size());
    }

    std::vector<CpuTexture> cpuTextures(textureTasks.size());
    std::atomic<size_t> textureCursor { 0 };
    const auto textureStart = Clock::now();
    workers.clear();
    const unsigned textureWorkers = (std::min)(available, static_cast<unsigned>(textureTasks.size()));
    for (unsigned w = 0; w < textureWorkers; ++w)
    {
        workers.emplace_back([&] {
            for (;;)
            {
                const size_t index = textureCursor.fetch_add(1);
                if (index >= textureTasks.size() || m_cancelled.load()) return;
                CpuTexture item;
                item.dependency = textureTasks[index];
                try
                {
                    item.decoded = Texture2D::Decode(Wide(item.dependency.path).c_str(), item.source,
                        item.dependency.kind == LevelDependencyKind::Environment
                            ? TextureDecodeMode::Environment : TextureDecodeMode::Color, &item.error);
                }
                catch (const std::exception& exception) { item.error = exception.what(); }
                cpuTextures[index] = std::move(item);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    AddTiming("Decode textures", Clock::now() - textureStart);
    {
        std::lock_guard lock(m_mutex);
        for (const auto& texture : cpuTextures)
            if (!texture.decoded)
                m_status.warnings.push_back({ LevelDiagnosticSeverity::Warning,
                    texture.dependency.path, "Optional dependency failed: " + texture.error });
    }
    PublishProgress(0.80f, textures.empty() ? path : textures.back().path);
    if (m_cancelled.load()) return;

    {
        std::lock_guard lock(m_mutex);
        m_cpuMeshes = std::move(cpuMeshes);
        m_cpuTextures = std::move(cpuTextures);
        m_prepared = std::move(prepared);
        m_status.state = LevelLoadState::Uploading;
        m_status.progress = (std::max)(m_status.progress, 0.80f);
    }
}

void LevelLoadOperation::PumpUploads(ID3D11Device* device, ID3D11DeviceContext* context,
                                     AssetManager& assets)
{
    (void)context;
    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();
    std::lock_guard lock(m_mutex);
    if (m_status.state != LevelLoadState::Uploading || !m_prepared) return;
    while (std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count() < 16.0)
    {
        if (m_cancelled.load()) { m_status.state = LevelLoadState::Cancelled; return; }
        if (m_meshUploadIndex < m_cpuMeshes.size())
        {
            auto& cpu = m_cpuMeshes[m_meshUploadIndex++];
            m_status.currentAsset = cpu.path;
            auto mesh = assets.FindMesh(cpu.path);
            if (!mesh)
            {
                mesh = std::make_shared<Mesh>();
                if (!mesh->Upload(device, std::move(cpu.source)))
                {
                    ++m_status.requiredFailures;
                    m_status.state = LevelLoadState::Failed;
                    m_status.failureReason = "Required mesh upload failed: " + cpu.path;
                    return;
                }
                assets.CacheMesh(cpu.path, mesh);
            }
            m_readyMeshes[cpu.path] = std::move(mesh);
        }
        else if (m_textureUploadIndex < m_cpuTextures.size())
        {
            auto& cpu = m_cpuTextures[m_textureUploadIndex++];
            m_status.currentAsset = cpu.dependency.path;
            if (cpu.decoded)
            {
                auto texture = assets.FindTexture(Wide(cpu.dependency.path));
                if (!texture)
                {
                    texture = std::make_shared<Texture2D>();
                    if (!texture->Upload(device, cpu.source)) texture.reset();
                    else assets.CacheTexture(Wide(cpu.dependency.path), texture);
                }
                if (texture)
                {
                    m_readyTextures[Wide(cpu.dependency.path)] = texture;
                    m_prepared->textures.push_back(std::move(texture));
                }
            }
        }
        else break;
        ++m_status.completedDependencies;
        m_status.progress = (std::max)(m_status.progress, 0.80f + 0.19f *
            static_cast<float>(m_status.completedDependencies) /
            static_cast<float>((std::max)(1u, m_status.dependencyCount)));
    }
    m_uploadMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
    if (m_meshUploadIndex < m_cpuMeshes.size() || m_textureUploadIndex < m_cpuTextures.size()) return;

    m_prepared->meshes.clear();
    m_prepared->materials.clear();
    for (const auto& descriptorMesh : m_prepared->descriptor.meshes)
    {
        const std::string path = LevelDependencyPlan::NormalizePath(descriptorMesh.path);
        const auto ready = m_readyMeshes.find(path);
        if (ready == m_readyMeshes.end() || !ready->second)
        {
            ++m_status.requiredFailures;
            m_status.state = LevelLoadState::Failed;
            m_status.failureReason = "Required cached mesh disappeared: " + path;
            return;
        }
        const auto& mesh = ready->second;
        m_prepared->meshes.push_back(mesh);
        std::vector<ForwardPipeline::SubMat> materials;
        materials.reserve(mesh->GetSubMeshCount());
        for (uint32_t i = 0; i < mesh->GetSubMeshCount(); ++i)
        {
            const auto info = mesh->GetSubMeshInfo(i);
            const auto resolve = [&](const std::string& raw) {
                if (raw.empty()) return AssetHandle<Texture2D>{};
                const std::string dds = StemDDS(raw);
                auto texture = FindTexture(assets, mesh->GetDirectory() + "Textures/" + dds);
                if (!texture) texture = FindTexture(assets, mesh->GetDirectory() + dds);
                return texture;
            };
            ForwardPipeline::SubMat material;
            material.albedo = resolve(info.albedoPath);
            material.normal = resolve(info.normalPath);
            material.roughness = resolve(info.roughnessPath);
            material.emissive = resolve(info.emissivePath);
            if (!material.albedo) material.albedo = assets.GetDefaultWhite();
            if (!material.normal) material.normal = assets.GetDefaultNormal();
            if (!material.roughness) material.roughness = assets.GetDefaultWhite();
            if (!material.emissive) material.emissive = assets.GetDefaultBlack();
            material.metallic = assets.GetDefaultBlack();
            material.alphaMode = info.alphaMode;
            material.alphaCutoff = info.alphaCutoff;
            materials.push_back(std::move(material));
        }
        m_prepared->materials.push_back(std::move(materials));
    }
    for (const auto& object : m_prepared->descriptor.objects)
        m_prepared->objectMaterials.push_back(ResolveObjectMaterial(assets, object));
    for (const auto& particle : m_prepared->descriptor.particles)
        m_prepared->particleTextures.push_back(FindTexture(assets, particle.texture));
    const std::string environmentPath = m_prepared->descriptor.skybox.empty()
        ? m_prepared->descriptor.reflectionPanorama : m_prepared->descriptor.skybox;
    m_prepared->environment = FindTexture(assets, environmentPath);
    if (!m_prepared->environment) m_prepared->environment = assets.GetDefaultBlack();
    m_status.timings.push_back({ "GPU uploads", m_uploadMilliseconds });
    m_prepared->diagnostics.insert(m_prepared->diagnostics.end(),
                                   m_status.warnings.begin(), m_status.warnings.end());
    m_prepared->timings = m_status.timings;
    m_cache = {};
    m_readyMeshes.clear();
    m_readyTextures.clear();
    m_status.progress = 1.0f;
    m_status.state = LevelLoadState::Complete;
}

LevelLoadStatus LevelLoadOperation::GetStatus() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

std::unique_ptr<PreparedLevel> LevelLoadOperation::TakePreparedLevel()
{
    std::lock_guard lock(m_mutex);
    if (m_status.state != LevelLoadState::Complete) return {};
    return std::move(m_prepared);
}

} // namespace SE
