// Basic.hlsl — forward PBR + equirectangular IBL + directional/point shadows.
// Registers: b0=TransformCB, b1=LightCB, b2=PointLightCB, b3=MaterialCB, b4=ForwardShadowCB
//            t0=albedo, t1=roughness, t2=normal, t3=shadowMap, t4=sky(IBL), t5-t6=pointShadow, t7=metallic
//            s0=sampler, s1=shadowSampler(cmp), s2=cubeSampler

cbuffer TransformCB : register(b0)
{
    row_major matrix Model;
    row_major matrix View;
    row_major matrix Projection;
    row_major matrix PrevModel;         // last frame's model matrix
    row_major matrix ViewProjNoJitter;  // current frame, no TAA jitter
    row_major matrix PrevViewProj;      // last frame, no TAA jitter
};

cbuffer LightCB : register(b1)
{
    float3 LightDir;      float  IBLIntensity;
    float3 LightColor;    float  DiffuseIBLSaturation;
    float3 CameraPos;     float  DebugLightMode;  // 0=normal, 1=force lit, 2=show NdotL
    row_major matrix LightViewProj;
};

struct PointLight { float3 Position; float Radius; float3 Color; float _pad; };
cbuffer PointLightCB : register(b2)
{
    PointLight PointLights[8];
    int        NumPointLights;
    float3     _plpad;
};

cbuffer MaterialCB : register(b3)
{
    float3 AlbedoTint;  float  RoughnessScale;
    float  Metallic;    float  Unlit;  float  DebugShadow;  float  AlphaCutoff;
    float  EmissiveIntensity;  float3 EmissiveColor;
    // Workflow: 0 = separate maps (t1.r rough, t7.r metal),
    //            1 = packed MR (t1 = x, roughness, metallic),
    //            2 = spec/gloss (t1.rgb specular color, t1.a glossiness)
    float  Workflow;    float  GlossinessFactor;  float ReceiveShadows; float _mcbPad;
};

cbuffer ForwardShadowCB : register(b4)
{
    int   NumPointShadowCasters;
    float PointShadowBias;
    float2 _fscbPad;
};

cbuffer SpotLightCB : register(b5)
{
    float3 SpotPosition;   float  SpotRange;
    float3 SpotDirection;  float  SpotOuterCos;
    float3 SpotColor;      float  SpotInnerCos;
    row_major matrix SpotViewProj;
    float  SpotShadowBias; int SpotEnabled; float2 _spotPad;
};

cbuffer CSMCB : register(b6)
{
    row_major matrix CascadeVP[3];
    float4 CascadeSplits;  // view-space far Z for each cascade
    float4 CascadeTexel;   // world-space texel size per cascade
};

cbuffer PlanarReflectionCB : register(b7)
{
    row_major matrix ReflectionViewProj;
    float PlaneY;
    float PlanarStrength;
    float PlanarEnabled;
    float ClipEnabled;
};

Texture2D              g_albedo       : register(t0);
Texture2D              g_roughness    : register(t1);
Texture2D              g_normal       : register(t2);
Texture2DArray         g_shadowMap    : register(t3);  // CSM cascade array
Texture2D              g_irradiance   : register(t4);
TextureCube<float>     g_pointShadow0 : register(t5);
TextureCube<float>     g_pointShadow1 : register(t6);
Texture2D              g_metallic     : register(t7);  // metallic map (R channel)
Texture2D              g_spotShadow   : register(t8);  // spot light shadow map
Texture2D              g_emissive     : register(t9);  // emissive map
Texture2D              g_prefilteredEnv : register(t10);
Texture2D              g_brdfLut      : register(t11);
Texture2D              g_planarReflection : register(t12);
SamplerState           g_sampler      : register(s0);
SamplerComparisonState g_shadowSampler: register(s1);
SamplerState           g_cubeSampler  : register(s2);  // linear-clamp for point shadows

struct VSIn
{
    float3 Position  : POSITION;
    float3 Normal    : NORMAL;
    float2 TexCoord  : TEXCOORD;
    float3 Tangent   : TANGENT;
    float3 Bitangent : BINORMAL;
};

struct PSIn
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD1;
    float2 TexCoord : TEXCOORD0;
    float3 T        : TEXCOORD2;
    float3 B        : TEXCOORD3;
    float3 N        : TEXCOORD4;
    float  ViewZ    : TEXCOORD5;
    float4 CurrClip : TEXCOORD6;  // jitter-free clip pos, current frame
    float4 PrevClip : TEXCOORD7;  // jitter-free clip pos, last frame
};

PSIn VS_Main(VSIn input)
{
    PSIn o;
    float4 world = mul(float4(input.Position, 1.0f), Model);
    o.WorldPos   = world.xyz;
    float4 viewPos = mul(world, View);
    o.ViewZ      = viewPos.z;
    o.Position   = mul(viewPos, Projection);
    o.CurrClip   = mul(world, ViewProjNoJitter);
    o.PrevClip   = mul(mul(float4(input.Position, 1.0f), PrevModel), PrevViewProj);
    o.TexCoord   = input.TexCoord;
    float3x3 m3  = (float3x3)Model;
    o.T = normalize(mul(input.Tangent,   m3));
    o.B = normalize(mul(input.Bitangent, m3));
    o.N = normalize(mul(input.Normal,    m3));
    return o;
}

// ---- PBR helpers ----

static const float PI = 3.14159265359f;

float NDF_GGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-5f);
}

float G_SchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / max(NdotX * (1.0f - k) + k, 1e-5f);
}

float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 r = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (r - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Cook-Torrance BRDF times NdotL (ready to multiply by lightColor).
float3 CookTorrance(float3 N, float3 V, float3 L,
                    float3 albedo, float roughness, float metallic, float3 F0)
{
    float3 H    = normalize(L + V);
    float NdotV = max(dot(N, V), 1e-4f);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float VdotH = max(dot(V, H), 0.0f);

    float  D = NDF_GGX(NdotH, roughness);
    float  G = G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);

    float3 spec = D * G * F / max(4.0f * NdotV * NdotL, 1e-5f);
    float3 kD   = (1.0f - F) * (1.0f - metallic);
    return (kD * albedo / PI + spec) * NdotL;
}

// ---- IBL helpers ----

float2 DirToEnvUV(float3 dir)
{
    return float2(atan2(dir.z, dir.x) * (0.5f / PI) + 0.5f,
                  0.5f - asin(clamp(dir.y, -1.0f, 1.0f)) / PI);
}

// IBL ambient:
//   Diffuse  — samples the panorama at max mip in the normal direction (approximate irradiance).
//   Specular — split-sum approximation: prefiltered env × BRDF(NdotV, roughness).
//              BRDF term uses Lazarov 2013 analytical fit to avoid a LUT texture.
float3 IBL(float3 N, float3 V, float3 worldPos, float3 albedo,
           float roughness, float metallic, float3 F0)
{
    float NdotV = max(dot(N, V), 0.0f);
    float3 F    = F_SchlickRoughness(NdotV, F0, roughness);
    float3 kD   = (1.0f - F) * (1.0f - metallic);

    // Diffuse: sample panorama at its maximum mip level (approximate average irradiance).
    float3 irradiance = g_irradiance.SampleLevel(g_sampler, DirToEnvUV(N), 0).rgb;
    float irradianceLuma = dot(irradiance, float3(0.2126f, 0.7152f, 0.0722f));
    irradiance = lerp(irradianceLuma.xxx, irradiance, saturate(DiffuseIBLSaturation));
    float3 diffuse    = kD * irradiance * albedo * IBLIntensity;

    // Specular: prefiltered env at roughness-scaled LOD × split-sum BRDF approximation.
    float3 R = reflect(-V, N);
    float envW, envH, envMips;
    g_prefilteredEnv.GetDimensions(0, envW, envH, envMips);
    float3 envColor = g_prefilteredEnv.SampleLevel(
        g_sampler, DirToEnvUV(R), roughness * (envMips - 1.0f)).rgb;

    if (PlanarEnabled > 0.5f)
    {
        float4 reflectedClip = mul(float4(worldPos, 1.0f), ReflectionViewProj);
        float2 reflectedUV = float2(reflectedClip.x / reflectedClip.w * 0.5f + 0.5f,
                                    -reflectedClip.y / reflectedClip.w * 0.5f + 0.5f);
        float2 edge = smoothstep(0.0f, 0.04f, reflectedUV) *
                      (1.0f - smoothstep(0.96f, 1.0f, reflectedUV));
        float confidence = edge.x * edge.y * saturate(PlanarStrength);
        float planarW, planarH, planarMips;
        g_planarReflection.GetDimensions(0, planarW, planarH, planarMips);
        float3 planar = g_planarReflection.SampleLevel(
            g_sampler, reflectedUV, roughness * (planarMips - 1.0f)).rgb;
        envColor = lerp(envColor, planar, confidence);
    }

    float2 brdf = g_brdfLut.SampleLevel(
        g_sampler, float2(NdotV, 1.0f - roughness), 0).rg;

    float3 specular = envColor * (F0 * brdf.x + brdf.y) * IBLIntensity;

    return diffuse + specular;
}

// ---- PS ----

struct PSOut
{
    float4 color    : SV_TARGET0;
    float4 normal   : SV_TARGET1;  // view-space normal (xyz) + roughness (w)
    float2 velocity : SV_TARGET2;  // UV-space motion, current - previous
};

float2 ComputeVelocity(float4 currClip, float4 prevClip)
{
    float2 currNDC = currClip.xy / currClip.w;
    float2 prevNDC = prevClip.xy / prevClip.w;
    return (currNDC - prevNDC) * float2(0.5f, -0.5f);
}

PSOut PS_Main(PSIn input)
{
    PSOut o;
    o.velocity = ComputeVelocity(input.CurrClip, input.PrevClip);
    if (ClipEnabled > 0.5f)
        clip(input.WorldPos.y - PlaneY - 0.01f);
    if (Unlit > 0.5f && Unlit < 1.5f)
    {
        o.color  = float4(AlbedoTint, 1.0f);
        o.normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return o;
    }

    // Normal map → world-space N
    float2 tbn_rg = g_normal.Sample(g_sampler, input.TexCoord).rg * 2.0f - 1.0f;
    float3 tbn_n  = float3(tbn_rg, sqrt(saturate(1.0f - dot(tbn_rg, tbn_rg))));
    float3x3 TBN  = float3x3(normalize(input.T), normalize(input.B), normalize(input.N));
    float3 N      = normalize(mul(tbn_n, TBN));

    float3 V        = normalize(CameraPos - input.WorldPos);
    float4 albedo   = g_albedo.Sample(g_sampler, input.TexCoord);
    albedo.rgb     *= AlbedoTint;

    // Alpha test (cutout): discard fragments below threshold
    if (AlphaCutoff > 0.0f)
        clip(albedo.a - AlphaCutoff);

    // Viewport overrides preserve alpha testing and never modify materials.
    // 2 = textured unlit, 3 = flat wireframe (rasterizer supplies triangle edges).
    if (Unlit > 1.5f)
    {
        float3 emission = g_emissive.Sample(g_sampler, input.TexCoord).rgb
                        * EmissiveColor * EmissiveIntensity;
        o.color = float4(Unlit > 2.5f ? float3(0.65f, 0.75f, 0.85f) : albedo.rgb + emission, albedo.a);
        o.normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return o;
    }

    // See MaterialCB Workflow comment for the t1 texture interpretations.
    float4 mrSample = g_roughness.Sample(g_sampler, input.TexCoord);
    float roughness;
    float metallic;
    float3 F0;
    if (Workflow > 1.5f) // spec/gloss: RGB drives F0 directly, no metal term
    {
        float gloss = saturate(mrSample.a * GlossinessFactor);
        roughness = max(saturate((1.0f - gloss) * RoughnessScale), 0.04f);
        metallic  = 0.0f;
        F0        = mrSample.rgb;
    }
    else
    {
        float roughSample = Workflow > 0.5f ? mrSample.g : mrSample.r;
        roughness = max(saturate(roughSample * RoughnessScale), 0.04f);
        float metalSample = Workflow > 0.5f ? mrSample.b
                                            : g_metallic.Sample(g_sampler, input.TexCoord).r;
        metallic  = saturate(metalSample * Metallic);
        F0        = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallic);
    }
    // Geometric specular AA: widen roughness where the shaded normal changes
    // faster than a pixel, so sub-pixel highlights stop shimmering (thin bars,
    // high-curvature edges). Kaplanyan-style normal-variance kernel.
    {
        float3 dndx = ddx(N);
        float3 dndy = ddy(N);
        float  variance = dot(dndx, dndx) + dot(dndy, dndy);
        float  kernelRoughness = min(2.0f * variance, 0.18f);
        roughness = sqrt(saturate(roughness * roughness + kernelRoughness));
    }

    // Cascaded directional shadow (PCF 3×3)
    float shadow = 1.0f;
    if (ReceiveShadows > 0.5f)
    {
        // Select cascade based on view-space depth
        int cascade = 2;
        if (input.ViewZ < CascadeSplits.x)      cascade = 0;
        else if (input.ViewZ < CascadeSplits.y)  cascade = 1;

        // Normal-offset receiver bias: push the sample point along the surface
        // normal by ~1 shadow texel so large cascades stop self-shadowing.
        float3 gN     = normalize(input.N);
        float  NdotLs = saturate(dot(gN, normalize(LightDir)));
        float  offset = CascadeTexel[cascade] * (2.0f - NdotLs);
        float3 samplePos = input.WorldPos + gN * offset;

        float4 sc   = mul(float4(samplePos, 1.0f), CascadeVP[cascade]);
        float3 ndc  = sc.xyz / sc.w;
        float2 uv   = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        float  dref = ndc.z - 0.0008f;
        if (saturate(uv.x) == uv.x && saturate(uv.y) == uv.y && dref < 1.0f)
        {
            float w, h, elements;
            g_shadowMap.GetDimensions(w, h, elements);
            float ts = 1.0f / w;
            shadow = 0.0f;
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            [unroll] for (int dy = -1; dy <= 1; ++dy)
                shadow += g_shadowMap.SampleCmpLevelZero(
                    g_shadowSampler, float3(uv + float2(dx, dy) * ts, (float)cascade), dref);
            shadow /= 9.0f;
        }
    }
    if (DebugLightMode > 0.5f) shadow = 1.0f;

    float3 L    = normalize(LightDir);
    float  NdotL = max(dot(N, L), 0.0f);

    // IBL ambient
    float3 color = IBL(N, V, input.WorldPos, albedo.rgb, roughness, metallic, F0);

    // Directional light (Cook-Torrance)
    color += shadow * LightColor * CookTorrance(N, V, L, albedo.rgb, roughness, metallic, F0);

    // Point lights
    for (int i = 0; i < NumPointLights; ++i)
    {
        float3 toL = PointLights[i].Position - input.WorldPos;
        float  d   = length(toL);
        if (d >= PointLights[i].Radius) continue;
        float  atten = 1.0f - (d / PointLights[i].Radius);
        atten        = atten * atten;

        float ptShadow = 1.0f;
        if (ReceiveShadows > 0.5f && i < NumPointShadowCasters)
        {
            float3 L2F   = input.WorldPos - PointLights[i].Position;
            float  sd    = length(L2F) / PointLights[i].Radius;
            float  clos  = (i == 0) ? g_pointShadow0.Sample(g_cubeSampler, L2F).r
                                    : g_pointShadow1.Sample(g_cubeSampler, L2F).r;
            ptShadow = (sd > clos + PointShadowBias) ? 0.0f : 1.0f;
        }

        float3 PL = toL / d;
        color += ptShadow * atten * PointLights[i].Color
               * CookTorrance(N, V, PL, albedo.rgb, roughness, metallic, F0);
    }

    // Spot light
    if (SpotEnabled > 0)
    {
        float3 toSpot = SpotPosition - input.WorldPos;
        float  dist   = length(toSpot);

        if (dist < SpotRange)
        {
            float3 SL    = toSpot / dist;
            float  cosA  = dot(-SL, normalize(SpotDirection));
            float  cone  = smoothstep(SpotOuterCos, SpotInnerCos, cosA);

            if (cone > 0.0f)
            {
                float distAtten = 1.0f - (dist / SpotRange);
                distAtten       = distAtten * distAtten;

                // Shadow (PCF 3×3)
                float spotShadow = 1.0f;
                if (ReceiveShadows > 0.5f)
                {
                    float4 sc  = mul(float4(input.WorldPos, 1.0f), SpotViewProj);
                    float3 ndc = sc.xyz / sc.w;
                    float2 uv  = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
                    float  dref = ndc.z;
                    if (saturate(uv.x) == uv.x && saturate(uv.y) == uv.y && dref < 1.0f)
                    {
                        float sw, sh; g_spotShadow.GetDimensions(sw, sh);
                        float ts = 1.0f / sw;
                        spotShadow = 0.0f;
                        [unroll] for (int sx = -1; sx <= 1; ++sx)
                        [unroll] for (int sy = -1; sy <= 1; ++sy)
                            spotShadow += g_spotShadow.SampleCmpLevelZero(
                                g_shadowSampler, uv + float2(sx, sy) * ts, dref - SpotShadowBias);
                        spotShadow /= 9.0f;
                    }
                }

                color += spotShadow * cone * distAtten * SpotColor
                       * CookTorrance(N, V, SL, albedo.rgb, roughness, metallic, F0);
            }
        }
    }

    if (DebugShadow > 0.5f)
    {
        o.color  = float4(shadow, shadow, shadow, 1.0f);
        o.normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return o;
    }
    if (DebugLightMode > 2.5f)
    {
        o.color  = float4(roughness.xxx, 1.0f);
        o.normal = float4(0.0f, 0.0f, 0.0f, roughness);
        return o;
    }
    if (DebugLightMode > 1.5f)
    {
        o.color  = float4(NdotL, NdotL, NdotL, 1.0f);
        o.normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return o;
    }

    // Emissive contribution (added directly to HDR color, feeds into bloom)
    float3 emissive = g_emissive.Sample(g_sampler, input.TexCoord).rgb;
    float emissiveSum = emissive.r + emissive.g + emissive.b;
    if (emissiveSum > 0.001f)
        color += emissive * EmissiveColor * EmissiveIntensity;

    // Store view-space normal and roughness for screen-space effects.
    float3 viewN = normalize(mul(N, (float3x3)View));

    o.color  = float4(color, albedo.a);
    o.normal = float4(viewN * 0.5f + 0.5f, roughness);
    return o;
}
