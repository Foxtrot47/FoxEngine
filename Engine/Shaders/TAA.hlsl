// Temporal Anti-Aliasing resolve.
// Blends the current HDR frame with reprojected history, using per-pixel
// motion vectors (velocity buffer) and a 3x3 neighborhood clamp to reject
// stale history. Sky pixels (depth == 1) reproject via camera matrices.

Texture2D    g_scene    : register(t0);  // current HDR color
Texture2D    g_history  : register(t1);  // accumulated HDR history
Texture2D    g_velocity : register(t2);  // UV-space motion vectors
Texture2D    g_depth    : register(t3);  // scene depth
SamplerState g_point    : register(s0);
SamplerState g_linear   : register(s1);

cbuffer TAACB : register(b0)
{
    row_major matrix InvViewProj;   // current frame, jitter-free
    row_major matrix PrevViewProj;  // last frame, jitter-free
    float2 RcpFrame;    // 1 / render target size
    float  BlendFactor; // history weight (e.g. 0.9)
    float  Reset;       // 1 = discard history this frame
    float  ClampGamma;  // variance clip width in stddevs (higher = calmer, more ghosting)
    float3 _pad;
};

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput VS_Main(float2 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOutput o;
    o.pos = float4(pos, 0.0f, 1.0f);
    o.uv  = uv;
    return o;
}

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// Resolve in tonemapped space so HDR fireflies cannot dominate the
// neighborhood statistics (inverted after blending).
float3 TonemapWeight(float3 c)   { return c / (1.0f + Luma(c)); }
float3 TonemapUnweight(float3 c) { return c / max(1.0f - Luma(c), 1e-4f); }

// Velocity of the closest (front-most) pixel in a 3x3 window so silhouette
// edges reproject with the foreground object.
float2 DilatedVelocity(float2 uv, out float closestDepth)
{
    float2 bestUV = uv;
    closestDepth  = 1.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        float2 sampleUV = uv + float2(x, y) * RcpFrame;
        float  d = g_depth.SampleLevel(g_point, sampleUV, 0).r;
        if (d < closestDepth)
        {
            closestDepth = d;
            bestUV = sampleUV;
        }
    }
    return g_velocity.SampleLevel(g_point, bestUV, 0).rg;
}

// Camera-only reprojection for pixels the geometry pass never touched (sky).
float2 SkyVelocity(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 world = mul(float4(ndc, 1.0f, 1.0f), InvViewProj);
    float4 prev  = mul(world, PrevViewProj);
    float2 prevNDC = prev.xy / prev.w;
    return (ndc - prevNDC) * float2(0.5f, -0.5f);
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv;
    float3 curr = g_scene.SampleLevel(g_point, uv, 0).rgb;

    float  closestDepth;
    float2 velocity = DilatedVelocity(uv, closestDepth);
    if (closestDepth >= 0.99999f)
        velocity = SkyVelocity(uv);

    float2 prevUV = uv - velocity;
    bool offscreen = any(prevUV < 0.0f) || any(prevUV > 1.0f);
    if (Reset > 0.5f || offscreen)
        return float4(curr, 1.0f);

    // Variance clipping in tonemapped space: build a mean/stddev AABB from the
    // 3x3 neighborhood — much tighter than min/max, rejects flickering history.
    float3 currT = TonemapWeight(curr);
    float3 m1 = currT;
    float3 m2 = currT * currT;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0) continue;
        float3 c = TonemapWeight(
            g_scene.SampleLevel(g_point, uv + float2(x, y) * RcpFrame, 0).rgb);
        m1 += c;
        m2 += c * c;
    }
    float3 mean  = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, 0.0f));
    float3 boxMin = mean - ClampGamma * sigma;
    float3 boxMax = mean + ClampGamma * sigma;

    float3 history = TonemapWeight(g_history.SampleLevel(g_linear, prevUV, 0).rgb);
    history = clamp(history, boxMin, boxMax);

    float3 result = lerp(currT, history, BlendFactor);
    return float4(TonemapUnweight(result), 1.0f);
}
