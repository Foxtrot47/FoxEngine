Texture2D tex : register(t0);
SamplerState splr : register(s0);

float4 main( float2 tc : TexCoord, float3 normal : Normal) : SV_Target
{
    float ambientStrength = 0.1;
    float3 lightDirection = { 0.0f, 0.0f, -1.0f }; // straight forward
    float3 lightColor = { 1.0f, 1.0f, 1.0f }; // white light
    float3 ambientLight = { 1.0f, 1.0f, 1.0f }; // white ambient light

    float3 ambient = ambientLight * ambientStrength;
    float3 diffuse = lightColor * max(0, dot(normalize(normal), normalize(lightDirection)));

    return float4(tex.Sample(splr, tc).rgb * (diffuse + ambient), 1.0f);
}
