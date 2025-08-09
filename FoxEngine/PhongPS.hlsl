cbuffer LightCBuffer : register(b0)
{
    float3 lightPos;
    float3 lightColor;
    float ambientStrength;
    float3 ambientLight;
    float specularIntensity;
    float specularPower;
};

cbuffer CamerCbuffer : register(b11)
{
    float3 camPos;
};

Texture2D tex : register(t0);
SamplerState splr : register(s0);

float4 main(float3 worldPos : Position, float3 normal : Normal, float2 tc : TexCoord) : SV_Target
{
    float3 N = normalize(normal);
    float3 vectorToLight = lightPos - worldPos;
    float distanceToLight = length(vectorToLight);
    float3 directionToLight = (distanceToLight > 1e-6f) ? (vectorToLight / distanceToLight) : float3(0.0f, 0.0f, 0.0f);
    
    float attenuation = 1.0f / (1.0f + 0.014f * distanceToLight + 0.0007f * (distanceToLight * distanceToLight));

    float3 ambient = ambientLight * ambientStrength;
    
    float3 diffuse = lightColor * attenuation * max(0.0f, dot(N, directionToLight));
    
    float3 reflectionDirection = reflect(-directionToLight, N);
    float3 vectorToCamera = normalize(camPos - worldPos);
    float spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    float3 specular = lightColor * specularIntensity * spec * attenuation;

    return tex.Sample(splr, tc) * float4(saturate(ambient + diffuse + specular), 1.0f);
}
