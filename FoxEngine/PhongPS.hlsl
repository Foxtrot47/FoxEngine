cbuffer LightCBuffer : register(b0) // global light properties
{
    float3 lightPos;
    float3 lightColor;
    float ambientStrength;
    float3 ambientLight;
    float globalSpecularIntensity;
};

cbuffer MaterialCBuffer : register(b1)  // Material properties
{
    float materialSpecularMask;
    float specularPower;
    float hasSpecularMap;
}

cbuffer CamerCbuffer : register(b11)
{
    float3 camPos;
};

Texture2D tex : register(t0);
Texture2D specularTex : register(t5);
SamplerState splr : register(s0);

float4 main(float3 worldPos : Position, float3 normal : Normal, float2 tc : TexCoord) : SV_Target
{
    float3 N = normalize(normal);
    float3 vectorToLight = lightPos - worldPos;
    float distanceToLight = length(vectorToLight);
    float3 directionToLight = (distanceToLight > 1e-6f) ? (vectorToLight / distanceToLight) : float3(0.0f, 0.0f, 0.0f);
    
    float attenuation = 1.0f / (1.0f + 0.0014f * distanceToLight + 0.000007f * (distanceToLight * distanceToLight));

    float3 ambient = ambientLight * ambientStrength;
    
    float3 diffuse = lightColor * attenuation * max(0.0f, dot(N, directionToLight));
    
    float3 reflectionDirection = reflect(-directionToLight, N);
    float3 vectorToCamera = normalize(camPos - worldPos);
    float spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    float finalSpecularIntensity = globalSpecularIntensity * materialSpecularMask;

    float3 specularColor;
    if (hasSpecularMap > 0.0f)
    {
        specularColor = specularTex.Sample(splr, tc).rgb * finalSpecularIntensity;
    }
    else
    {
        specularColor = float3(finalSpecularIntensity, finalSpecularIntensity, finalSpecularIntensity);
    }
    
    float3 specular = lightColor * globalSpecularIntensity * specularColor * spec * attenuation;

    return tex.Sample(splr, tc) * float4(saturate(ambient + diffuse + specular), 1.0f);
}
