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
    float materialSpecularMask;
    float specularPower;
    float hasSpecularMap;
}

cbuffer CamerCbuffer : register(b11)
{
    float3 camPos;
};

Texture2D tex : register(t0);
Texture2D normalTex : register(t1);
SamplerState splr : register(s0);

struct PSIn
{
    float3 worldPos : Position;
    float3 normal : Normal;
    float2 tex : TexCoord;
    float4 pos : SV_Position;
    float3 tangent : Tangent;
    float3 bitangent : BiTangent;
};

float4 main(PSIn input) : SV_Target
{
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    
    float3x3 TBN = float3x3(T, B, N);
    
    float3 normalMapSample = normalTex.Sample(splr, input.tex).rgb;
        
    // Convert from [0,1] color range to [-1,1] direction range
    float3 tangentSpaceNormal = normalMapSample * 2.0f - 1.0f;

    float3 finalNormal = normalize(mul(tangentSpaceNormal, TBN));

    float3 vectorToLight = lightPos - input.worldPos;
    float distanceToLight = length(vectorToLight);
    float3 directionToLight = (distanceToLight > 1e-6f) ? (vectorToLight / distanceToLight) : float3(0.0f, 0.0f, 0.0f);
    
    float attenuation = 1.0f / (1.0f + 0.0014f * distanceToLight + 0.000007f * (distanceToLight * distanceToLight));
    float3 ambient = ambientLight * ambientStrength;
    
    float3 diffuse = lightColor * attenuation * max(0.0f, dot(finalNormal, directionToLight));
    
    float3 reflectionDirection = reflect(-directionToLight, finalNormal);
    float3 vectorToCamera = normalize(camPos - input.worldPos);
    float spec = pow(max(0.0f, dot(reflectionDirection, vectorToCamera)), specularPower);
    
    float3 specular = lightColor * globalSpecularIntensity * materialSpecularMask * spec * attenuation;
    return tex.Sample(splr, input.tex) * float4(saturate(ambient + diffuse + specular), 1.0f);
}