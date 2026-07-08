// VolumetricFog.hlsl — single-scattering volumetric fog + directional light shafts.
// Ray-marches from the camera to the reconstructed scene depth, sampling the
// cascaded sun shadow map per step. Output: RGB = in-scattered light, A = transmittance.
// Composited into the HDR buffer as: Final = inScatter + hdr * transmittance.

Texture2D                 g_depth     : register(t0);
Texture2DArray            g_shadow    : register(t1);
Texture2D                 g_sky       : register(t2);
SamplerState              g_point     : register(s0);
SamplerComparisonState    g_shadowCmp : register(s1);
SamplerState              g_skySampler: register(s2);

static const float PI = 3.14159265359f;

cbuffer FogCB : register(b0)
{
    row_major matrix InvViewProj;
    row_major matrix View;
    row_major matrix CascadeVP[3];
    float4 CascadeSplits;   // x,y = far view-Z of cascade 0,1
    float3 CameraPos;   float Density;
    float3 LightDir;    float Anisotropy;       // g (forward scattering)
    float3 LightColor;  float StepCount;
    float3 FogAmbient;  float AmbientIntensity;
    float2 ScreenSize;  float MaxDistance; float ScatterIntensity;
    float  HeightFalloff; float HeightBase; float NoiseStrength; float NoiseScale;
    float  NoiseTime; float _pad0; float _pad1; float _pad2;
};

struct VSOutput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput VS_Main(float2 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOutput o;
    o.pos = float4(pos, 0.0f, 1.0f);
    o.uv  = uv;
    return o;
}

// Game-friendly forward scattering. A physically raw HG lobe gets extremely bright
// near the sun and reads as white sheets instead of atmospheric haze.
float PhaseHG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    float hg = (1.0f - g2) / (4.0f * PI * pow(max(denom, 1e-4f), 1.5f));
    float isotropic = 1.0f / (4.0f * PI);
    return min(hg, isotropic * 6.0f);
}

float DistanceFogWeight(float dist, float maxDistance)
{
    float x = saturate(dist / max(maxDistance, 1.0f));
    return smoothstep(0.05f, 0.85f, x);
}

float2 DirToEnvUV(float3 dir)
{
    dir = normalize(dir);
    if (dot(dir.xz, dir.xz) < 1e-6f)
        dir.x = 1e-3f;
    return float2(atan2(dir.z, dir.x) * (0.5f / PI) + 0.5f,
                  0.5f - asin(clamp(dir.y, -1.0f, 1.0f)) / PI);
}

float3 SampleSkyAmbient(float3 rayDir)
{
    float w, h, mips;
    g_sky.GetDimensions(0, w, h, mips);
    float mip = max(mips - 1.0f, 0.0f);

    float3 upColor = g_sky.SampleLevel(g_skySampler, DirToEnvUV(float3(0.0f, 1.0f, 0.0f)), mip).rgb;
    float3 rayColor = g_sky.SampleLevel(g_skySampler, DirToEnvUV(rayDir), mip).rgb;
    float horizon = 1.0f - abs(normalize(rayDir).y);
    return lerp(upColor, rayColor, 0.35f + 0.35f * horizon);
}

// Interleaved gradient noise — per-pixel dither to break ray-march banding.
float IGN(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float Hash13(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);

    float n000 = Hash13(i + float3(0, 0, 0));
    float n100 = Hash13(i + float3(1, 0, 0));
    float n010 = Hash13(i + float3(0, 1, 0));
    float n110 = Hash13(i + float3(1, 1, 0));
    float n001 = Hash13(i + float3(0, 0, 1));
    float n101 = Hash13(i + float3(1, 0, 1));
    float n011 = Hash13(i + float3(0, 1, 1));
    float n111 = Hash13(i + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

float FogNoise(float3 worldPos)
{
    if (NoiseStrength <= 0.001f || NoiseScale <= 0.0001f)
        return 1.0f;

    float3 p = worldPos * NoiseScale + float3(NoiseTime * 0.07f, NoiseTime * 0.015f, -NoiseTime * 0.05f);
    float n = ValueNoise(p) * 0.55f + ValueNoise(p * 2.13f + 19.7f) * 0.30f + ValueNoise(p * 4.03f - 7.1f) * 0.15f;
    float contrast = smoothstep(0.18f, 0.92f, n);
    return lerp(1.0f, lerp(0.35f, 1.65f, contrast), saturate(NoiseStrength));
}

// Reconstruct world position from screen UV + non-linear depth.
float3 WorldPosFromDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    ndc.y = -ndc.y;
    float4 wp = mul(ndc, InvViewProj);
    return wp.xyz / wp.w;
}

// Samples one explicit cascade's shadow map at a world-space point.
// Returns (visibility, valid). Invalid lookups are not treated as lit; the
// caller can try another cascade or conservatively suppress sun in-scatter.
float2 SampleCascade(int cascade, float3 worldPos)
{
    float4 sc = mul(float4(worldPos, 1.0f), CascadeVP[cascade]);
    if (abs(sc.w) < 1e-5f)
        return float2(0.0f, 0.0f);

    float3 ndc  = sc.xyz / sc.w;
    float2 uv   = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    float  dref = ndc.z;

    if (saturate(uv.x) != uv.x || saturate(uv.y) != uv.y || dref <= 0.0f || dref >= 1.0f)
        return float2(0.0f, 0.0f);

    float w, h, layers;
    g_shadow.GetDimensions(w, h, layers);
    float2 texel = 1.0f / float2(w, h);

    float visibility = 0.0f;
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        visibility += g_shadow.SampleCmpLevelZero(
            g_shadowCmp, float3(uv + float2(dx, dy) * texel, (float)cascade), dref);
    }

    return float2(visibility / 9.0f, 1.0f);
}

// Sun visibility from the cascaded shadow map at a world-space point.
// Volumetric samples must be conservative: an invalid cascade lookup should not
// inject light, otherwise shafts appear through occluders at cascade edges.
float SampleSunVisibility(float3 worldPos)
{
    float viewZ = mul(float4(worldPos, 1.0f), View).z;

    int cascade = 2;
    if (viewZ < CascadeSplits.x)      cascade = 0;
    else if (viewZ < CascadeSplits.y) cascade = 1;

    float2 result = SampleCascade(cascade, worldPos);
    if (result.y > 0.5f)
        return result.x;

    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (c == cascade) continue;
        result = SampleCascade(c, worldPos);
        if (result.y > 0.5f)
            return result.x;
    }

    return 0.0f;
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float rawDepth = g_depth.SampleLevel(g_point, input.uv, 0).r;

    // Ray from camera through this pixel.
    float3 worldPos = WorldPosFromDepth(input.uv, min(rawDepth, 0.999999f));
    float3 toFrag   = worldPos - CameraPos;
    float  sceneDist = length(toFrag);
    float3 rayDir    = (sceneDist > 1e-5f) ? toFrag / sceneDist : float3(0, 0, 1);

    // Sky pixels: march the full distance.
    float marchDist = (rawDepth >= 1.0f) ? MaxDistance : min(sceneDist, MaxDistance);

    int   steps    = max((int)StepCount, 1);
    float stepLen  = marchDist / (float)steps;
    float dither   = IGN(input.pos.xy);

    float cosTheta = dot(LightDir, rayDir);
    float phase    = PhaseHG(cosTheta, Anisotropy);

    float3 skyAmbient = SampleSkyAmbient(rayDir) * FogAmbient;

    float3 inScatter    = float3(0, 0, 0);
    float  transmittance = 1.0f;

    float3 pos = CameraPos + rayDir * (dither * stepLen);
    float marchOffset = dither * stepLen;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        // Height-based density falloff (optional).
        float heightAtten = exp(-max(pos.y - HeightBase, 0.0f) * HeightFalloff);
        float d = Density * heightAtten * FogNoise(pos);
        float stepOptical = d * stepLen;

        float dist = marchOffset + (float)i * stepLen;
        float farFog = DistanceFogWeight(dist, MaxDistance);
        float vis = SampleSunVisibility(pos);

        float3 ambientFog = skyAmbient * AmbientIntensity * lerp(0.65f, 1.35f, farFog);
        float shaftMask = smoothstep(0.15f, 0.95f, vis);
        float shaftFade = saturate(1.0f - farFog * 0.35f);
        float3 sunShaft = LightColor * (phase * ScatterIntensity) * shaftMask * shaftFade;
        float3 lit = ambientFog + sunShaft;

        inScatter += transmittance * lit * stepOptical;
        transmittance *= exp(-stepOptical);

        pos += rayDir * stepLen;

        if (transmittance < 0.003f) break;
    }

    return float4(inScatter, transmittance);
}
