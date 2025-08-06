cbuffer CBuf
{
    matrix transform;
}

struct VSOut
{
    float2 tex : TexCoord;
    float3 normal : Normal;
    float4 pos : SV_Position;
};

VSOut main(float3 pos : Position, float3 normal : Normal, float2 tex : TexCoord)
{
    VSOut vso;
    vso.pos = mul(float4(pos, 1.0f), transform);
    vso.tex = tex;
    vso.normal = mul(normal, (float3x3)transform);
    return vso;
}