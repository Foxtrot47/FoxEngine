#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/MeshCache.h"
#include "Engine/Renderer/ForwardPipeline.h"
#include "Engine/Renderer/Mesh.h"

namespace SE {

enum class MeshAddState { Idle, Preparing, Uploading, Complete, Failed };

// Async single-mesh import for adding a mesh to a live scene: geometry import
// and texture decode run on a worker thread, GPU uploads are pumped on the
// main thread (same split as LevelLoadOperation, scoped to one mesh).
class MeshAddOperation
{
public:
    struct Result
    {
        std::string path;
        AssetHandle<Mesh> mesh;
        std::vector<ForwardPipeline::SubMat> materials;
    };

    MeshAddOperation() = default;
    ~MeshAddOperation();
    MeshAddOperation(const MeshAddOperation&) = delete;
    MeshAddOperation& operator=(const MeshAddOperation&) = delete;

    bool Start(const std::string& path, AssetCacheSnapshot cache);
    void PumpUploads(ID3D11Device* device, AssetManager& assets);
    MeshAddState GetState() const;
    std::string GetError() const;
    std::string GetPath() const;
    std::unique_ptr<Result> TakeResult();

private:
    struct CpuTexture
    {
        std::string path;
        TextureSourceData source;
        bool decoded = false;
    };

    void Join();
    void Prepare();

    mutable std::mutex m_mutex;
    std::thread m_worker;
    std::atomic<bool> m_cancelled { false };
    MeshAddState m_state = MeshAddState::Idle;
    std::string m_path;
    std::string m_error;
    AssetCacheSnapshot m_cache;
    MeshCache m_meshCache;
    MeshSourceData m_meshSource;
    bool m_needsMeshUpload = false;
    bool m_meshUploaded = false;
    std::vector<CpuTexture> m_cpuTextures;
    size_t m_textureUploadIndex = 0;
    std::unique_ptr<Result> m_result;
};

} // namespace SE
