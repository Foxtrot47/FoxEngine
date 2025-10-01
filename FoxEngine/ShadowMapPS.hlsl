cbuffer PerObject : register(b0)
{
    matrix modelMatrix;
    float3 lightPosition;
    float lightRange;
    int lightIndex;
    int faceIndex;
    float2 padding;
}

struct PSInput
{
    float4 pos : SV_Position;
    float3 worldPos : Position;
};

float main(PSInput input) : SV_Depth
{
    float3 lightToFragment = input.worldPos - lightPosition;
    float distance = length(lightToFragment);
    return saturate(distance / lightRange);
}