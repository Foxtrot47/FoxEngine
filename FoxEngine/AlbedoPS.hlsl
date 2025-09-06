cbuffer LightCBuffer : register(b0) // global light properties
{
    float3 lightPos;
    float3 lightColor;
    float ambientStrength;
    float3 ambientLight;
    float globalSpecularIntensity;
};

cbuffer MaterialCBuffer : register(b1) // Material properties
{
    float3 albedoColor;
    float materialSpecularMask;
    float specularPower;
    float hasSpecularMap;
}
float4 main() : SV_TARGET
{
    return float4(albedoColor, 1.0f);
}