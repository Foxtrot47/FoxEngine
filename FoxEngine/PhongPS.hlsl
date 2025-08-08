cbuffer LightCBuffer
{
    float3 lightPos;
    float3 lightColor;
};

Texture2D tex : register(t0);
SamplerState splr : register(s0);

float4 main(float3 worldPos : Position, float3 normal : Normal, float2 tc : TexCoord) : SV_Target
{
    float ambientStrength = 0.1;
    float3 ambientLight = { 1.0f, 1.0f, 1.0f }; // white ambient light
    
    float3 vectorToLight = lightPos - worldPos;
    float distanceToLight = length(vectorToLight);
    float3 directionToLight = vectorToLight / distanceToLight;
    float attenuation = 1 / (1.0f + 0.014 * distanceToLight + 0.0007 * (distanceToLight * distanceToLight));

    float3 ambient = ambientLight * ambientStrength;
    float3 diffuse = lightColor * attenuation * max(0.0f, dot(directionToLight, normal));

    return float4(tex.Sample(splr, tc).rgb * (diffuse + ambient), 1.0f);
}
