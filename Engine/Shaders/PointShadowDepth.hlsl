// Point light omnidirectional shadow depth pass.
// Writes linear distance / LightFar into an R32_FLOAT cube face (colour target).
#include "Billboard.hlsli"

cbuffer PointShadowCB : register(b0)
{
    row_major matrix ViewProj;  // cube face view-projection
    row_major matrix World;
    float3 LightPos;
    float  LightFar;
};

cbuffer ShadowMaterialCB : register(b1)
{
    float  AlphaCutoff;
    float  Billboard;  // 1: cylindrical billboard, the card turns toward Facing
    float2 _pad;
    float4 Facing;     // w = 0: xyz world direction toward the light; w = 1: xyz light world position
};

Texture2D    g_albedo      : register(t0);
SamplerState g_alphaSampler: register(s0);

struct VSIn
{
    float3 Position  : POSITION;
    float3 Normal    : NORMAL;
    float2 TexCoord  : TEXCOORD;
    float3 Tangent   : TANGENT;
    float3 Bitangent : BINORMAL;
    float2 TexCoord2 : TEXCOORD2;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

VSOut VS_Main(VSIn input)
{
    VSOut o;
    float4 world = mul(float4(input.Position, 1.0f), World);
    if (Billboard > 0.5f)
    {
        const float3 toward = Facing.w > 0.5f ? Facing.xyz - world.xyz : Facing.xyz;
        world.xyz = BillboardExpand(world.xyz, BillboardFacing(toward).right, input.TexCoord2);
    }
    o.worldPos   = world.xyz;
    o.pos        = mul(world, ViewProj);
    o.uv         = input.TexCoord;
    return o;
}

float PS_Main(VSOut input) : SV_Target0
{
    if (AlphaCutoff > 0.0f)
        clip(g_albedo.SampleLevel(g_alphaSampler, input.uv, 0).a - AlphaCutoff);
    return length(input.worldPos - LightPos) / LightFar;
}
