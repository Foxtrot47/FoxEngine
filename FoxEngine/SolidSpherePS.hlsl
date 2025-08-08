cbuffer LightCBuffer
{
    float3 lightPos;
    float3 lightColor;
};

float4 main() : SV_TARGET
{
    return float4(lightColor, 1.0f);
}