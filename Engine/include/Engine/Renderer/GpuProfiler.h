#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace SE {

// GPU timestamp timings of named render scopes. Each frame brackets its scopes in a
// TIMESTAMP_DISJOINT query; results are read without flushing up to k_frameLatency
// frames later and smoothed with an exponential moving average (alpha 0.1).
class GpuProfiler
{
public:
    static constexpr uint32_t k_frameLatency = 4;

    // scopeNames: the scopes Begin/End accept, by index; the pointers must outlive
    // the profiler (string literals).
    bool Init(ID3D11Device* device, const std::vector<const char*>& scopeNames);
    void Shutdown();

    // Brackets one frame. A frame whose ring slot is still waiting for the GPU is
    // not timed (its scopes are ignored). BeginFrame ends an unfinished frame first.
    void BeginFrame(ID3D11DeviceContext* ctx);
    void EndFrame(ID3D11DeviceContext* ctx);

    // At most one Begin/End pair per scope and frame; scopes must not nest.
    void Begin(ID3D11DeviceContext* ctx, uint32_t scope);
    void End(ID3D11DeviceContext* ctx, uint32_t scope);

    // Scope name -> smoothed GPU milliseconds; a scope that did not run in a
    // frame contributes 0 ms to its average.
    const std::vector<std::pair<const char*, float>>& GetTimings() const { return m_timings; }
    // Smoothed GPU milliseconds from BeginFrame to EndFrame.
    float GetFrameMs() const { return m_frameMs; }

    // Scope that ends when it leaves its block.
    class Scope
    {
    public:
        Scope(GpuProfiler& profiler, ID3D11DeviceContext* ctx, uint32_t scope)
            : m_profiler(profiler), m_ctx(ctx), m_scope(scope) { m_profiler.Begin(m_ctx, m_scope); }
        ~Scope() { m_profiler.End(m_ctx, m_scope); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        GpuProfiler&         m_profiler;
        ID3D11DeviceContext* m_ctx;
        uint32_t             m_scope;
    };

private:
    struct Frame
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, frameBegin, frameEnd;
        std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> begin, end; // per scope
        std::vector<uint8_t> issued; // per scope: both timestamps issued this frame
        bool pending = false;        // submitted, results not read yet
    };

    // Reads every completed pending frame, oldest first, into the averages.
    void Collect(ID3D11DeviceContext* ctx);

    std::array<Frame, k_frameLatency> m_frames;
    uint32_t m_writeIndex = 0;  // frame being recorded
    uint32_t m_readIndex  = 0;  // oldest pending frame
    bool     m_recording  = false; // BeginFrame issued and not ended
    bool     m_timed      = false; // the recording frame has queries
    bool     m_haveSample = false;
    std::vector<std::pair<const char*, float>> m_timings;
    float    m_frameMs = 0.0f;
};

} // namespace SE
