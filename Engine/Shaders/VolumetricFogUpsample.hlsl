// VolumetricFogUpsample.hlsl — depth-aware upsample of the half-resolution
// volumetric fog buffer to full resolution.
//
// The fog march runs at half resolution for performance. A naive bilinear
// upsample bleeds fog across depth discontinuities (haloing around pillars and
// silhouettes), so each full-res pixel re-weights the four nearest low-res fog
// texels by how closely their scene depth matches the full-res depth here.
// Output keeps the fog encoding (RGB = in-scatter, A = transmittance) and is
// composited into the HDR buffer by the caller's blend state.

Texture2D    g_fog   : register(t0); // half-res fog
Texture2D    g_depth : register(t1); // full-res scene depth
SamplerState g_point : register(s0);

cbuffer UpsampleCB : register(b0)
{
    float2 LowResSize;
    float2 InvLowResSize;
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

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float zFull = g_depth.SampleLevel(g_point, input.uv, 0).r;

    // Locate the 2x2 low-res neighbourhood around this pixel.
    float2 p    = input.uv * LowResSize - 0.5f;
    float2 base = floor(p);
    float2 f    = p - base;
    float2 uv00 = (base + 0.5f) * InvLowResSize;

    const float2 halfTexel = 0.5f * InvLowResSize;
    const float2 offsets[4] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f),
        float2(0.0f, 1.0f), float2(1.0f, 1.0f)
    };
    const float bilinear[4] = {
        (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
        (1.0f - f.x) * f.y,          f.x * f.y
    };

    float4 result = float4(0, 0, 0, 0);
    float  total  = 0.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float2 uvi = clamp(uv00 + offsets[i] * InvLowResSize,
                           halfTexel, 1.0f - halfTexel);

        // Depth the low-res texel marched against (its centre maps onto the
        // full-res depth buffer). Taps on the far side of a silhouette get a
        // large depth mismatch and drop out of the blend.
        float zi = g_depth.SampleLevel(g_point, uvi, 0).r;
        float w  = bilinear[i] / (1e-5f + abs(zFull - zi));

        result += g_fog.SampleLevel(g_point, uvi, 0) * w;
        total  += w;
    }

    return result / max(total, 1e-5f);
}
