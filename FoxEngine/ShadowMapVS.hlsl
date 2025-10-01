cbuffer PerObject : register(b0)
{
    matrix modelMatrix;
    float3 lightPosition;
    float lightRange;
    int lightIndex;
    int faceIndex;
    float2 padding;
}

cbuffer LightShadowMatrices : register(b1)
{
    matrix lightViewProj[5][6];
    float3 lightPositions[5];
}

struct VSIn
{
    float3 pos : Position;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 worldPos : Position;
};

VSOut main(VSIn input)
{
    VSOut output;
    float4 worldPos = mul(float4(input.pos, 1.0f), modelMatrix);
    output.worldPos = worldPos.xyz;
    output.pos = mul(worldPos, lightViewProj[lightIndex][faceIndex]);
    return output;
}