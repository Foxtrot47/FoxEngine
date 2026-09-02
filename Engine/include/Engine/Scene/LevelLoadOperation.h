#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Scene/LevelAsset.h"
#include "Engine/Scene/LevelDependencyPlan.h"
#include "Engine/Scene/SceneDescriptor.h"

namespace SE {

enum class LevelLoadState { Idle, Preparing, Uploading, Complete, Failed, Cancelled };

struct LevelStageTiming
{
    std::string name;
    double milliseconds = 0.0;
};

struct LevelLoadStatus
{
    LevelLoadState state = LevelLoadState::Idle;
    float progress = 0.0f;
    std::string currentAsset;
    uint32_t completedDependencies = 0;
    uint32_t dependencyCount = 0;
    uint32_t cacheHits = 0;
    uint32_t requiredFailures = 0;
    std::vector<LevelDiagnostic> warnings;
    std::string failureReason;
    std::vector<LevelStageTiming> timings;
};

struct PreparedLevel
{
    LevelAsset level;
    SceneDescriptor descriptor;
    LevelDependencyPlan plan;
    std::vector<AssetHandle<Mesh>> meshes;
    std::vector<std::vector<ForwardPipeline::SubMat>> materials;
    std::vector<ForwardPipeline::SubMat> objectMaterials;
    std::vector<AssetHandle<Texture2D>> particleTextures;
    AssetHandle<Texture2D> environment;
    AssetHandle<Texture2D> iblEnvironment; // reflectionPanorama override, else == environment
    std::vector<AssetHandle<Texture2D>> textures;
    std::vector<LevelDiagnostic> diagnostics;
    std::vector<LevelStageTiming> timings;
};

class LevelLoadOperation
{
public:
    LevelLoadOperation() = default;
    ~LevelLoadOperation();
    LevelLoadOperation(const LevelLoadOperation&) = delete;
    LevelLoadOperation& operator=(const LevelLoadOperation&) = delete;

    bool Start(const std::string& path, AssetCacheSnapshot cache);
    void Cancel();
    void PumpUploads(ID3D11Device* device, ID3D11DeviceContext* context,
                     AssetManager& assets);
    LevelLoadStatus GetStatus() const;
    std::unique_ptr<PreparedLevel> TakePreparedLevel();

private:
    struct CpuMesh { std::string path; MeshSourceData source; };
    struct CpuTexture
    {
        LevelDependency dependency;
        TextureSourceData source;
        bool decoded = false;
        std::string error;
    };

    void Join();
    void Prepare(std::string path);
    void SetTerminal(LevelLoadState state, const std::string& reason = {});
    void PublishProgress(float value, const std::string& current = {});
    void AddTiming(const char* name, std::chrono::steady_clock::duration elapsed);

    mutable std::mutex m_mutex;
    std::thread m_coordinator;
    std::atomic<bool> m_cancelled { false };
    LevelLoadStatus m_status;
    std::unique_ptr<PreparedLevel> m_prepared;
    std::vector<CpuMesh> m_cpuMeshes;
    std::vector<CpuTexture> m_cpuTextures;
    AssetCacheSnapshot m_cache;
    std::unordered_map<std::string, AssetHandle<Mesh>> m_readyMeshes;
    std::unordered_map<std::wstring, AssetHandle<Texture2D>> m_readyTextures;
    size_t m_meshUploadIndex = 0;
    size_t m_textureUploadIndex = 0;
    double m_uploadMilliseconds = 0.0;
};

} // namespace SE
