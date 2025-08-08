cbuffer CBuf
{
    matrix modelMatrix;
    matrix modelViewProjection;
}

struct VSOut
{
    float3 worldPos : Position; 
    float3 normal : Normal;
    float2 tex : TexCoord;
    float4 pos : SV_Position;
};

VSOut main(float3 pos : Position, float3 normal : Normal, float2 tex : TexCoord)
{
    VSOut vso;
    vso.pos = mul(float4(pos, 1.0f), modelViewProjection);
    vso.tex = tex;
    vso.normal = mul(normal, (float3x3)modelMatrix);
    vso.worldPos = (float3) mul(float4(pos, 1.0f), modelMatrix);
    return vso;
}