// VolumetricFogBlur.hlsl - depth-aware separable Gaussian blur for the fog buffer.
// RGB = in-scattered light, A = transmittance; taps across scene-depth edges are rejected.

Texture2D    g_source  : register(t0);
Texture2D    g_depth   : register(t1);
SamplerState g_point   : register(s0);

cbuffer BlurCB : register(b0)
{
    float2 TexelSize;   // 1/width, 1/height
    float2 Direction;   // (1,0) horizontal or (0,1) vertical
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

float DepthEdgeWeight(float centerDepth, float sampleDepth)
{
    bool centerSky = centerDepth >= 0.999999f;
    bool sampleSky = sampleDepth >= 0.999999f;
    if (centerSky != sampleSky)
        return 0.0f;

    float threshold = max(0.0005f, (1.0f - min(centerDepth, sampleDepth)) * 0.01f);
    return (abs(centerDepth - sampleDepth) <= threshold) ? 1.0f : 0.0f;
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    const float w[5] = { 0.227027f, 0.194594f, 0.121621f, 0.054054f, 0.016216f };
    float2 step = Direction * TexelSize;

    float centerDepth = g_depth.SampleLevel(g_point, input.uv, 0).r;
    float4 sum = g_source.SampleLevel(g_point, input.uv, 0) * w[0];
    float totalWeight = w[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        float2 off = step * (float)i;

        float2 uvP = input.uv + off;
        float depthP = g_depth.SampleLevel(g_point, uvP, 0).r;
        float weightP = w[i] * DepthEdgeWeight(centerDepth, depthP);
        sum += g_source.SampleLevel(g_point, uvP, 0) * weightP;
        totalWeight += weightP;

        float2 uvN = input.uv - off;
        float depthN = g_depth.SampleLevel(g_point, uvN, 0).r;
        float weightN = w[i] * DepthEdgeWeight(centerDepth, depthN);
        sum += g_source.SampleLevel(g_point, uvN, 0) * weightN;
        totalWeight += weightN;
    }

    return sum / max(totalWeight, 1e-5f);
}