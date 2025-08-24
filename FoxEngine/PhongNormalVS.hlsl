cbuffer CBuf
{
    matrix modelMatrix;
    matrix modelViewProjection;
}

struct VSIn
{
    float3 pos : Position;
    float3 normal : Normal;
    float2 tex : TexCoord;
    float4 tangent : Tangent;
};

struct VSOut
{
    float3 worldPos : Position;
    float3 normal : Normal;
    float2 tex : TexCoord;
    float4 pos : SV_Position;
    float3 tangent : Tangent;
    float3 bitangent : BiTangent;
};

VSOut main(VSIn input)
{
    VSOut vso;
    vso.pos = mul(float4(input.pos, 1.0f), modelViewProjection);
    vso.tex = input.tex;
    vso.normal = mul(input.normal, (float3x3) modelMatrix);
    vso.worldPos = (float3) mul(float4(input.pos, 1.0f), modelMatrix);
    vso.tangent = mul(input.tangent.xyz, (float3x3) modelMatrix);
    vso.bitangent = mul(cross(input.normal, input.tangent.xyz) * input.tangent.w, (float3x3) modelMatrix);
    return vso;
}