struct PointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct DirectionalLight
{
    float3 direction;
    float padding0;
    float3 color;
    float intensity;
};

cbuffer LightCBuffer : register(b0) // global light properties
{
    DirectionalLight directionalLight;
    PointLight pointLights[5];
    float3 ambientLight;
    int activeLightCount;
    float globalSpecularIntensity;
    float2 padding;
};

cbuffer LightShadowMatrices : register(b1)
{
    matrix directionalLightViewProj;
    matrix pointLightViewProj[5];
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

Texture2D pointShadowMap[5] : register(t4);
Texture2D directionalShadowMap : register(t9);
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

float CalculateDirectionalLightShadow(float3 worldPos)
{
    // Transform world position into light space
    float4 lightSpacePos = mul(float4(worldPos, 1.0f), directionalLightViewProj);

    // Perspective divide
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Texture coords in [0,1]
    float2 shadowTexCoord;
    shadowTexCoord.x = projCoords.x * 0.5f + 0.5f;
    shadowTexCoord.y = -projCoords.y * 0.5f + 0.5f; // Flip Y!
    
    // Check if outside shadow map bounds
    if (shadowTexCoord.x < 0.0f || shadowTexCoord.x > 1.0f ||
      shadowTexCoord.y < 0.0f || shadowTexCoord.y > 1.0f ||
      projCoords.z > 1.0f || projCoords.z < 0.0f)
    {
        return 1.0f; // Assume lit if outside shadow map
    }

    // Improved bias to prevent self-shadowing
    // Use larger bias to eliminate shadow acne under the light
    float bias = 0.005f;
    float currentDepth = projCoords.z - bias;
    
    // Clamp depth to valid range
    currentDepth = saturate(currentDepth);
    
    // 0 = in shadow, 1 = lit
    return directionalShadowMap.SampleCmpLevelZero(shadowSampler, shadowTexCoord.xy, currentDepth);
}

float CalculatePointShadow(float3 worldPos, int lightIndex)
{
    // Transform world position into light space
    float4 lightSpacePos = mul(float4(worldPos, 1.0f), pointLightViewProj[lightIndex]);

    // Perspective divide
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Texture coords in [0,1]
    float2 shadowTexCoord;
    shadowTexCoord.x = projCoords.x * 0.5f + 0.5f;
    shadowTexCoord.y = -projCoords.y * 0.5f + 0.5f; // Flip Y!
    
    // Check if outside shadow map bounds
    if (shadowTexCoord.x < 0.0f || shadowTexCoord.x > 1.0f ||
      shadowTexCoord.y < 0.0f || shadowTexCoord.y > 1.0f ||
      projCoords.z > 1.0f || projCoords.z < 0.0f)
    {
        return 1.0f; // Assume lit if outside shadow map
    }

    // Improved bias to prevent self-shadowing
    // Use larger bias to eliminate shadow acne under the light
    float bias = 0.005f;
    float currentDepth = projCoords.z - bias;
    
    // Clamp depth to valid range
    currentDepth = saturate(currentDepth);
    
    // 0 = in shadow, 1 = lit
    return pointShadowMap[lightIndex].SampleCmpLevelZero(shadowSampler, shadowTexCoord, currentDepth);
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

float3 CalculateDirectionalLightContribution(float3 worldPos, float3 normal, float3 viewDir, float3 materialColor, float materialSpecular)
{
    float3 lightDir;
    float attenuation = 1.0f;
    
    lightDir = normalize(-directionalLight.direction);

    float3 diffuse = directionalLight.color * directionalLight.intensity * max(0.0f, dot(normal, lightDir));
        
    float3 reflectionDirection = reflect(-lightDir, normal);
    float3 vectorToCamera = normalize(viewDir - worldPos);
    float3 spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    
    float3 specular = directionalLight.color * globalSpecularIntensity * materialSpecularMask;
    float shadowFactor = CalculateDirectionalLightShadow(worldPos);
    
    return (diffuse + specular) * shadowFactor;
}

float3 CalculatePointLightContribution(int index, float3 worldPos, float3 normal, float3 viewDir, float3 materialColor, float materialSpecular)
{
    float3 vectorToLight = pointLights[index].position - worldPos;
    float distanceToLight = length(vectorToLight);
    float lightDir = vectorToLight / distanceToLight;
    float attenuation = CalculateAttenuation(distanceToLight, pointLights[index].range);

    float3 diffuse = pointLights[index].color * pointLights[index].intensity * attenuation * max(0.0f, dot(normal, lightDir));
        
    float3 reflectionDirection = reflect(-lightDir, normal);
    float3 vectorToCamera = normalize(viewDir - worldPos);
    float3 spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    
    float3 specular = pointLights[index].color * globalSpecularIntensity * materialSpecularMask * attenuation;
	float shadowFactor = CalculatePointShadow(worldPos, index);
    
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
    
    finalColor += CalculateDirectionalLightContribution(
        input.worldPos,
        finalNormal,
        viewDir,
        albedo.rgb,
        materialSpecularMask) * albedo.rgb;;
    
    for (int i = 0; i < activeLightCount; ++i)
    {
        finalColor += CalculatePointLightContribution(
        i,
        input.worldPos,
        finalNormal,
        viewDir,
        albedo.rgb,
        materialSpecularMask);
    }
    return float4(saturate(finalColor), albedo.a);
}