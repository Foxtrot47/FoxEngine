#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include "Engine/Renderer/Texture2D.h"

namespace Editor {

// Async thumbnail loader for the content browser: a worker thread decodes and
// downscales image files to small RGBA bitmaps, Update() turns finished
// bitmaps into GPU textures on the main thread.
class ThumbnailCache
{
public:
    ThumbnailCache() = default;
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Returns the thumbnail SRV, or nullptr while it loads or when decoding
    // failed. The first call for a path queues it on the worker thread.
    ID3D11ShaderResourceView* Get(const std::string& path);
    void Update(ID3D11Device* device, ID3D11DeviceContext* ctx);

private:
    struct CpuThumb
    {
        std::string path;
        std::vector<uint8_t> rgba;
        uint32_t width = 0;
        uint32_t height = 0;
        bool ok = false;
    };

    void WorkerLoop();

    std::unordered_map<std::string, std::unique_ptr<SE::Texture2D>> m_textures;
    std::unordered_map<std::string, bool> m_requested; // true once GPU-resident or failed

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;
    std::vector<CpuThumb> m_finished;
    std::thread m_worker;
    std::atomic<bool> m_stop { false };
};

} // namespace Editor
