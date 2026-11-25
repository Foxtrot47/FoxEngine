#include "Engine/Renderer/GpuProfiler.h"
#include "Engine/Core/Logger.h"
#include <algorithm>

namespace SE {

namespace {

constexpr float k_smoothing = 0.1f;
// Frame::issued states per scope.
constexpr uint8_t k_scopeBegun = 1, k_scopeEnded = 2;

} // namespace

bool GpuProfiler::Init(ID3D11Device* device, const std::vector<const char*>& scopeNames)
{
    Shutdown();
    D3D11_QUERY_DESC disjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC stampDesc = { D3D11_QUERY_TIMESTAMP, 0 };
    const auto create = [device](const D3D11_QUERY_DESC& desc, Microsoft::WRL::ComPtr<ID3D11Query>& query) {
        return SUCCEEDED(device->CreateQuery(&desc, query.GetAddressOf()));
    };
    for (Frame& frame : m_frames)
    {
        bool ok = create(disjointDesc, frame.disjoint) && create(stampDesc, frame.frameBegin) &&
                  create(stampDesc, frame.frameEnd);
        frame.begin.resize(scopeNames.size());
        frame.end.resize(scopeNames.size());
        frame.issued.assign(scopeNames.size(), 0);
        for (size_t s = 0; ok && s < scopeNames.size(); ++s)
            ok = create(stampDesc, frame.begin[s]) && create(stampDesc, frame.end[s]);
        if (!ok)
        {
            SE_LOG_ERROR("GpuProfiler: timestamp query creation failed");
            Shutdown();
            return false;
        }
    }
    m_timings.clear();
    for (const char* name : scopeNames)
        m_timings.emplace_back(name, 0.0f);
    return true;
}

void GpuProfiler::Shutdown()
{
    for (Frame& frame : m_frames)
        frame = Frame{};
    m_timings.clear();
    m_writeIndex = m_readIndex = 0;
    m_recording = m_timed = m_haveSample = false;
    m_frameMs = 0.0f;
}

void GpuProfiler::BeginFrame(ID3D11DeviceContext* ctx)
{
    if (!m_frames[0].disjoint)
        return;
    if (m_recording)
        EndFrame(ctx);
    m_recording = true;
    Frame& frame = m_frames[m_writeIndex];
    if (frame.pending)
        Collect(ctx);
    m_timed = !frame.pending;
    if (!m_timed)
        return;
    std::fill(frame.issued.begin(), frame.issued.end(), uint8_t(0));
    ctx->Begin(frame.disjoint.Get());
    ctx->End(frame.frameBegin.Get());
}

void GpuProfiler::EndFrame(ID3D11DeviceContext* ctx)
{
    if (!m_recording)
        return;
    m_recording = false;
    if (m_timed)
    {
        Frame& frame = m_frames[m_writeIndex];
        ctx->End(frame.frameEnd.Get());
        ctx->End(frame.disjoint.Get());
        frame.pending = true;
        m_writeIndex = (m_writeIndex + 1) % k_frameLatency;
        m_timed = false;
    }
    Collect(ctx);
}

void GpuProfiler::Begin(ID3D11DeviceContext* ctx, uint32_t scope)
{
    if (!m_timed)
        return;
    Frame& frame = m_frames[m_writeIndex];
    ctx->End(frame.begin[scope].Get());
    frame.issued[scope] = k_scopeBegun;
}

void GpuProfiler::End(ID3D11DeviceContext* ctx, uint32_t scope)
{
    if (!m_timed)
        return;
    Frame& frame = m_frames[m_writeIndex];
    if (frame.issued[scope] != k_scopeBegun)
        return;
    ctx->End(frame.end[scope].Get());
    frame.issued[scope] = k_scopeEnded;
}

void GpuProfiler::Collect(ID3D11DeviceContext* ctx)
{
    const auto read = [ctx](ID3D11Query* query, uint64_t& value) {
        return ctx->GetData(query, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    };
    while (m_frames[m_readIndex].pending)
    {
        Frame& frame = m_frames[m_readIndex];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        if (ctx->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;
        uint64_t frameBegin = 0, frameEnd = 0;
        if (!read(frame.frameBegin.Get(), frameBegin) || !read(frame.frameEnd.Get(), frameEnd))
            return;
        // Scope stamps were issued before the frame end, so they are complete too.
        const bool valid = !disjoint.Disjoint && disjoint.Frequency > 0;
        const double toMs = valid ? 1000.0 / static_cast<double>(disjoint.Frequency) : 0.0;
        for (size_t s = 0; valid && s < m_timings.size(); ++s)
        {
            float ms = 0.0f;
            uint64_t begin = 0, end = 0;
            if (frame.issued[s] == k_scopeEnded && read(frame.begin[s].Get(), begin) &&
                read(frame.end[s].Get(), end) && end >= begin)
                ms = static_cast<float>(static_cast<double>(end - begin) * toMs);
            float& average = m_timings[s].second;
            average = m_haveSample ? average + k_smoothing * (ms - average) : ms;
        }
        if (valid)
        {
            const float ms = frameEnd >= frameBegin
                ? static_cast<float>(static_cast<double>(frameEnd - frameBegin) * toMs) : 0.0f;
            m_frameMs = m_haveSample ? m_frameMs + k_smoothing * (ms - m_frameMs) : ms;
            m_haveSample = true;
        }
        frame.pending = false;
        m_readIndex = (m_readIndex + 1) % k_frameLatency;
    }
}

} // namespace SE
