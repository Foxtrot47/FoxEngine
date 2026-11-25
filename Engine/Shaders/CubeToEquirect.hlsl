// CubeToEquirect.hlsl — resamples a captured reflection probe cube into the
// equirectangular layout used by the IBL maps (same mapping as EnvironmentFilter).
TextureCube  Source        : register(t0);
SamplerState LinearSampler : register(s1); // s0 is the fullscreen quad's point sampler

struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOutput VS_Main(float2 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOutput o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

static const float PI = 3.14159265359;

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float longitude = (input.uv.x - 0.5) * 2.0 * PI;
    float latitude = (0.5 - input.uv.y) * PI;
    float cosLat = cos(latitude);
    float3 dir = float3(cosLat * cos(longitude), sin(latitude), cosLat * sin(longitude));
    return float4(Source.SampleLevel(LinearSampler, dir, 0).rgb, 1.0);
}
