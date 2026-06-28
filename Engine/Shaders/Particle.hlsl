// Particle billboard shader.
// Per-vertex: quad corner (POSITION) + UV (TEXCOORD0)
// Per-instance: world position + size (TEXCOORD1), color (TEXCOORD2)
// Camera-facing billboards constructed in the vertex shader.

Texture2D    g_particleTex : register(t0);
SamplerState g_sampler     : register(s0);

cbuffer ParticleCameraCB : register(b0)
{
    row_major float4x4 ViewProj;
    float3 CamRight;
    float  _pad0;
    float3 CamUp;
    float  _pad1;
};

struct VSInput
{
    // Per-vertex (quad)
    float2 cornerPos : POSITION;
    float2 uv        : TEXCOORD0;
    // Per-instance
    float4 posSize   : TEXCOORD1;   // xyz = world pos, w = size
    float4 color     : TEXCOORD2;
};

struct PSInput
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

PSInput VS_Main(VSInput input)
{
    PSInput o;

    float3 center = input.posSize.xyz;
    float  size   = input.posSize.w;

    // Expand quad corner into world space billboard
    float3 worldPos = center
                    + CamRight * (input.cornerPos.x * size)
                    + CamUp    * (input.cornerPos.y * size);

    o.pos   = mul(float4(worldPos, 1.0f), ViewProj);
    o.uv    = input.uv;
    o.color = input.color;
    return o;
}

float4 PS_Main(PSInput input) : SV_TARGET
{
    float4 tex = g_particleTex.Sample(g_sampler, input.uv);
    return tex * input.color;
}
