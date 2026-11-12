// Shadow depth pass — renders to a depth texture from the light's perspective.
// Cutout materials alpha-test against albedo so foliage casts a shaped shadow.
cbuffer ShadowTransformCB : register(b0)
{
    row_major matrix ShadowModel;
    row_major matrix ShadowViewProj;
};

cbuffer ShadowMaterialCB : register(b1)
{
    float AlphaCutoff;
    float3 _pad;
};

Texture2D    g_albedo      : register(t0);
SamplerState g_alphaSampler: register(s0);

struct VSIn
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent  : TANGENT;
    float3 Bitangent: BINORMAL;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VS_Main(VSIn input)
{
    VSOut o;
    float4 world = mul(float4(input.Position, 1.0f), ShadowModel);
    o.pos = mul(world, ShadowViewProj);
    o.uv  = input.TexCoord;
    return o;
}

void PS_Main(VSOut input)
{
    if (AlphaCutoff > 0.0f)
        clip(g_albedo.SampleLevel(g_alphaSampler, input.uv, 0).a - AlphaCutoff);
}
