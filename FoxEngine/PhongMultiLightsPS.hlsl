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

cbuffer LightShadowMatrices : register(b1)
{
    matrix lightViewProj; // one matrix for now
};

cbuffer MaterialCBuffer : register(b2) // Material properties
{
    float materialSpecularMask;
    float specularPower;
    float hasSpecularMap;
}

cbuffer CamerCbuffer : register(b3)
{
    float3 camPos;
};

Texture2D tex : register(t0);
Texture2D normalTex : register(t1);
SamplerState splr : register(s0);

Texture2D shadowMap : register(t4);
SamplerComparisonState shadowSampler : register(s1);

struct PSIn
{
    float3 worldPos : Position;
    float3 normal : Normal;
    float2 tex : TexCoord;
    float4 pos : SV_Position;
    float3 tangent : Tangent;
    float3 bitangent : BiTangent;
};

float CalculateShadows(float3 worldPos)
{
    // Transform world position into light space
    float4 lightSpacePos = mul(float4(worldPos, 1.0f), lightViewProj);

    // Perspective divide
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Texture coords in [0,1]
    float2 shadowTexCoord;
    shadowTexCoord.x = projCoords.x * 0.5f + 0.5f;
    shadowTexCoord.y = -projCoords.y * 0.5f + 0.5f; // Flip Y!
    
    // Check if outside shadow map bounds
    if (shadowTexCoord.x < 0.0f || shadowTexCoord.x > 1.0f ||
      shadowTexCoord.y < 0.0f || shadowTexCoord.y > 1.0f)
    {
        return 1.0f; // Assume lit if outside shadow map
    }

    // 0 = in shadow, 1 = lit
    return shadowMap.SampleCmpLevelZero(shadowSampler, shadowTexCoord, projCoords.z);
}

float CalculateAttenuation(float distance, float range)
{
    // Quadratic attenuation with range cutoff
    if (distance > range)
        return 0.0f;
    
    float attenuation = 1.0f / (1.0f + 0.09f * distance + 0.032f * distance * distance);
    
    // Smooth cutoff at range boundary
    float rangeFactor = 1.0f - pow(distance / range, 4.0f);
    rangeFactor = max(rangeFactor, 0.0f);
    rangeFactor = rangeFactor * rangeFactor;
    
    return attenuation * rangeFactor;
}

float3 CalculateLightContribution(Light light, float3 worldPos, float3 normal, float3 viewDir, float3 materialColor,  float materialSpecular)
{
    float3 lightDir;
    float attenuation = 1.0f;
    
    if (light.type == 0) // Point Light
    {
        float3 vectorToLight = light.position - worldPos;
        float distanceToLight = length(vectorToLight);
        lightDir = vectorToLight / distanceToLight;
        attenuation = CalculateAttenuation(distanceToLight, light.range);
    }
    else if (light.type == 1)   // Directional Light
    {
        lightDir = normalize(-light.direction);
        attenuation = 1.0f; // No attenuation for directional lights
    }
    float3 diffuse = light.color  * light.intensity * attenuation * max(0.0f, dot(normal, lightDir));
        
    float3 reflectionDirection = reflect(-lightDir, normal);
    float3 vectorToCamera = normalize(viewDir - worldPos);
    float3 spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    
    float3 specular = light.color * globalSpecularIntensity * materialSpecularMask *  attenuation;
    float shadowFactor = CalculateShadows(worldPos);
    
    return (diffuse + specular) * shadowFactor;
}

float4 main(PSIn input) : SV_Target
{
    float4 albedo = tex.Sample(splr, input.tex);
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    
    float3x3 TBN = float3x3(T, B, N);
    
    float3 normalMapSample = normalTex.Sample(splr, input.tex).rgb;
        
    // Convert from [0,1] color range to [-1,1] direction range
    float3 tangentSpaceNormal = normalMapSample * 2.0f - 1.0f;

    float3 finalNormal = normalize(mul(tangentSpaceNormal, TBN));
   
    float3 viewDir = normalize(camPos - input.worldPos);
    float3 finalColor = albedo.rgb * ambientLight;
    
    for (int i = 0; i < activeLightCount; ++i)
    {
        finalColor += CalculateLightContribution(
        lights[i],
        input.worldPos,
        finalNormal,
        viewDir,
        albedo.rgb,
        materialSpecularMask) *
        albedo.rgb;

    }
    return float4(saturate(finalColor), albedo.a);
}