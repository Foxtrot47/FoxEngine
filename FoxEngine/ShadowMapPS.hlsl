struct PSInput
{
    float4 pos : SV_Position;
    float3 worldPos : Position;
};

struct Light
{
    float3 position;
    int type;
    float3 direction;
    float range;
    float3 color;
    float intensity;
};

cbuffer LightCBuffer : register(b0) // global light properties
{
    Light lights[5];
    float3 ambientLight;
    int activeLightCount;
    float globalSpecularIntensity;
    float3 padding;
};

cbuffer PerObjectBuffer : register(b1)
{
    matrix modelMatrix;
    int faceIndex;
    int lightIndex;
}

float main(PSInput input) : SV_Depth
{
    float3 lightToFragment = input.worldPos - lights[lightIndex].position;
    float distance = length(lightToFragment);
    return saturate(distance / lights[lightIndex].range);
}