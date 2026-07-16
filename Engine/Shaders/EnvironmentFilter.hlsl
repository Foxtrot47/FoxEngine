Texture2D Source : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer FilterCB : register(b0)
{
    float Roughness;
    int Mode;
    int SampleCount;
    float SourceMipCount;
};

struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOutput VS_Main(float2 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOutput o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

static const float PI = 3.14159265359;

float RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint count) { return float2(float(i) / count, RadicalInverse(i)); }

float3 UVToDirection(float2 uv)
{
    float longitude = (uv.x - 0.5) * 2.0 * PI;
    float latitude = (0.5 - uv.y) * PI;
    float cosLat = cos(latitude);
    return float3(cosLat * cos(longitude), sin(latitude), cosLat * sin(longitude));
}

float2 DirectionToUV(float3 d)
{
    d = normalize(d);
    return float2(atan2(d.z, d.x) * (0.5 / PI) + 0.5,
                  0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI);
}

float3 TangentSampleToWorld(float3 sampleDir, float3 n)
{
    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    return tangent * sampleDir.x + bitangent * sampleDir.y + n * sampleDir.z;
}

float3 ImportanceGGX(float2 xi, float roughness, float3 n)
{
    float a = max(roughness * roughness, 0.001);
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return normalize(TangentSampleToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), n));
}

float GeometrySchlick(float ndotv, float roughness)
{
    float k = roughness * roughness * 0.5;
    return ndotv / max(ndotv * (1.0 - k) + k, 1e-5);
}

float2 IntegrateBRDF(float ndotv, float roughness)
{
    float3 v = float3(sqrt(saturate(1.0 - ndotv * ndotv)), 0, ndotv);
    float a = 0, b = 0;
    [loop] for (uint i = 0; i < (uint)SampleCount; ++i)
    {
        float3 h = ImportanceGGX(Hammersley(i, SampleCount), roughness, float3(0, 0, 1));
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = saturate(l.z);
        float ndoth = saturate(h.z);
        float vdoth = saturate(dot(v, h));
        if (ndotl > 0)
        {
            float g = GeometrySchlick(ndotv, roughness) * GeometrySchlick(ndotl, roughness);
            float visibility = g * vdoth / max(ndoth * ndotv, 1e-5);
            float fc = pow(1.0 - vdoth, 5.0);
            a += (1.0 - fc) * visibility;
            b += fc * visibility;
        }
    }
    return float2(a, b) / SampleCount;
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    if (Mode == 2)
        return float4(IntegrateBRDF(saturate(input.uv.x), saturate(1.0 - input.uv.y)), 0, 1);

    float3 n = UVToDirection(input.uv);
    float3 sum = 0;
    float weight = 0;
    if (Mode == 1)
    {
        [loop] for (uint i = 0; i < (uint)SampleCount; ++i)
        {
            float2 xi = Hammersley(i, SampleCount);
            float phi = 2.0 * PI * xi.x;
            float cosTheta = sqrt(1.0 - xi.y);
            float sinTheta = sqrt(xi.y);
            float3 l = TangentSampleToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), n);
            float sourceLod = min(2.0, SourceMipCount - 1.0);
            sum += Source.SampleLevel(LinearSampler, DirectionToUV(l), sourceLod).rgb;
        }
        return float4(sum / SampleCount, 1);
    }

    if (SampleCount <= 1)
        return float4(Source.SampleLevel(LinearSampler, input.uv, 0).rgb, 1);
    float3 v = n;
    [loop] for (uint j = 0; j < (uint)SampleCount; ++j)
    {
        float3 h = ImportanceGGX(Hammersley(j, SampleCount), Roughness, n);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = saturate(dot(n, l));
        if (ndotl > 0)
        {
            sum += Source.SampleLevel(LinearSampler, DirectionToUV(l), Roughness * (SourceMipCount - 1.0)).rgb * ndotl;
            weight += ndotl;
        }
    }
    return float4(sum / max(weight, 1e-4), 1);
}
