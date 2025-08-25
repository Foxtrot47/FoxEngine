cbuffer CBuf
{
    matrix modelMatrix;
    matrix modelViewProjection;
}

struct VSOut
{
    float4 worldPos : Position;
    float2 tex : TexCoord;
    float4 pos : SV_Position;
};

VSOut main(float3 pos : Position, float2 tex : TexCoord)
{
    VSOut vso;
    vso.pos = mul(float4(pos, 1.0f), modelViewProjection);
    vso.pos.z = vso.pos.w;  // Ensure skybox is at farthest plane
    vso.tex = tex;
    return vso;
}