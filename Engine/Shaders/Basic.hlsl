// Basic.hlsl — forward PBR + equirectangular IBL + directional/point shadows.
// Registers: b0=TransformCB, b1=LightCB, b2=PointLightCB, b3=MaterialCB, b4=ForwardShadowCB,
//            b9=ReflectionProbeCB; VS t29=instance buffer (instanced draws)
//            t0=albedo, t1=roughness, t2=normal, t3=shadowMap, t4=sky(IBL), t5-t6=pointShadow, t7=metallic,
//            t13=occlusion, t14=detail albedo, t15=detail normal,
//            FOX_TERRAIN only: t16-t19=layer albedo A-D, t20=splat, t21=overlay, t22-t25=layer normal A-D,
//            t26=clearcoat, t27=clearcoat roughness, t28=reflection probe array
//            s0=sampler, s1=shadowSampler(cmp), s2=cubeSampler
// FOX_TERRAIN=1 (layered terrain permutation) replaces albedo, roughness and normal sampling.
// VS reads b1 (CameraPos) and b3 (BillboardParams) for cylindrical billboards.

#ifndef FOX_TERRAIN
#define FOX_TERRAIN 0
#endif

#include "Billboard.hlsli"

cbuffer TransformCB : register(b0)
{
    row_major matrix Model;
    row_major matrix View;
    row_major matrix Projection;
    row_major matrix PrevModel;         // last frame's model matrix
    row_major matrix ViewProjNoJitter;  // current frame, no TAA jitter
    row_major matrix PrevViewProj;      // last frame, no TAA jitter
    uint  InstanceBase;                 // first g_instances element of an instanced draw
    uint  UseInstances;                 // 1: Model/PrevModel come from g_instances
    uint2 _transformPad;
};

// Per-instance transforms of instanced draws (ForwardPipeline::InstanceData).
struct InstanceData
{
    row_major matrix Model;
    row_major matrix PrevModel;
};
StructuredBuffer<InstanceData> g_instances : register(t29);

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
    float  Workflow;    float  GlossinessFactor;  float ReceiveShadows; float OcclusionStrength;
    // Per-slot UV set and tiling, slots: 0 albedo, 1 normal, 2 roughness (and t7 metallic),
    // 3 emissive, 4 occlusion, 5 detail albedo, 6 detail normal.
    float4 SlotST[7];   // xy scale, zw offset: uv * scale + offset
    float4 SlotUVA;     // UV set (0..2 = TEXCOORD0..2, 3 = world X/Z) of slots 0-3
    float4 SlotUVB;     // UV set of slots 4-6, w = DetailNormalStrength
    float4 SlotFlags;   // x hasOcclusion, y hasDetailAlbedo, z hasDetailNormal, w occlusionChannel (0 R, 1 G)
    float4 TintParams;  // x = vertex tint mode (0 none, 1 multiply, 2 albedoAlphaMask), y = detail albedo mode (0 overlay, 1 multiply, 2 alphaBlend), z = detail albedo strength, w = overlay neutral scale (1 / neutral)
    float4 TerrainScale;   // world X/Z scale of terrain layers A-D
    float4 TerrainParams;  // x layerCount, y albedoAlphaIsRoughness, z overlayStrength, w overlay uvSet
    float4 TerrainParams2; // x splat uvSet
    float4 TerrainST;      // xy splat uv scale, zw overlay uv scale
    float4 Clearcoat;      // x factor, y roughness, z maskFromAlbedoAlpha, w hasTextures bitmask (1 coat, 2 rough)
    float4 ClearcoatST[2]; // 0 coat, 1 coat roughness: xy scale, zw offset
    float4 ClearcoatUV;    // x coat uv set, y coat-roughness uv set
    float4 BillboardParams; // x = 1 cylindrical billboard (VS: TexCoord2 is the card offset), yzw unused
};

cbuffer ForwardShadowCB : register(b4)
{
    int   NumPointShadowCasters;
    float PointShadowBias;
    float FrameIndex;         // dither animation step for cutout coverage
    float CutoutDitherWidth;  // 0 disables the dithered cutout threshold
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

// Box-projected reflection probes (ReflectionProbes): GGX-prefiltered equirect slices.
cbuffer ReflectionProbeCB : register(b9)
{
    float4 ProbeCenter[8];
    float4 ProbeExtents[8];  // xyz half extents, w blendDistance
    float4 ProbeInfo;        // x count, y mip count
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
Texture2D              g_occlusion    : register(t13);
Texture2D              g_detailAlbedo : register(t14);
Texture2D              g_detailNormal : register(t15);
#if FOX_TERRAIN
Texture2D              g_terrainAlbedo0 : register(t16);
Texture2D              g_terrainAlbedo1 : register(t17);
Texture2D              g_terrainAlbedo2 : register(t18);
Texture2D              g_terrainAlbedo3 : register(t19);
Texture2D              g_terrainSplat   : register(t20);
Texture2D              g_terrainOverlay : register(t21);
Texture2D              g_terrainNormal0 : register(t22);
Texture2D              g_terrainNormal1 : register(t23);
Texture2D              g_terrainNormal2 : register(t24);
Texture2D              g_terrainNormal3 : register(t25);
#endif
Texture2D              g_clearcoat          : register(t26);
Texture2D              g_clearcoatRoughness : register(t27);
Texture2DArray         g_reflectionProbes   : register(t28);
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
    float2 TexCoord1 : TEXCOORD1;
    float2 TexCoord2 : TEXCOORD2;
    float4 Color     : COLOR0;
    uint   InstanceID : SV_InstanceID;
};

struct PSIn
{
    float4 Position  : SV_POSITION;
    float3 WorldPos  : TEXCOORD1;
    float2 TexCoord  : TEXCOORD0;
    float3 T         : TEXCOORD2;
    float3 B         : TEXCOORD3;
    float3 N         : TEXCOORD4;
    float  ViewZ     : TEXCOORD5;
    float4 CurrClip  : TEXCOORD6;  // jitter-free clip pos, current frame
    float4 PrevClip  : TEXCOORD7;  // jitter-free clip pos, last frame
    float2 TexCoord1 : TEXCOORD8;
    float2 TexCoord2 : TEXCOORD9;
    float4 Color     : COLOR0;
};

PSIn VS_Main(VSIn input)
{
    PSIn o;
    float4x4 model = Model, prevModel = PrevModel;
    if (UseInstances != 0)
    {
        const InstanceData instance = g_instances[InstanceBase + input.InstanceID];
        model     = instance.Model;
        prevModel = instance.PrevModel;
    }
    float4 world     = mul(float4(input.Position, 1.0f), model);
    float4 prevWorld = mul(float4(input.Position, 1.0f), prevModel);
    float3x3 m3      = (float3x3)model;
    if (BillboardParams.x > 0.5f)
    {
        // Turn the card about +Y toward the camera; last frame's base uses this frame's facing.
        const BillboardFrame frame = BillboardFacing(CameraPos - world.xyz);
        const float2 offset = input.TexCoord2;
        world.xyz     = BillboardExpand(world.xyz, frame.right, offset);
        prevWorld.xyz = BillboardExpand(prevWorld.xyz, frame.right, offset);
        o.T = frame.right;
        o.B = float3(0.0f, 1.0f, 0.0f);
        // Rounded canopy normal, so cards facing away from the sun are not black.
        o.N = normalize(frame.f + 0.6f * (frame.right * sign(offset.x) +
                                          float3(0.0f, offset.y > 0.0f ? 1.0f : -1.0f, 0.0f)));
    }
    else
    {
        o.T = normalize(mul(input.Tangent,   m3));
        o.B = normalize(mul(input.Bitangent, m3));
        o.N = normalize(mul(input.Normal,    m3));
    }
    o.WorldPos   = world.xyz;
    float4 viewPos = mul(world, View);
    o.ViewZ      = viewPos.z;
    o.Position   = mul(viewPos, Projection);
    o.CurrClip   = mul(world, ViewProjNoJitter);
    o.PrevClip   = mul(prevWorld, PrevViewProj);
    o.TexCoord   = input.TexCoord;
    o.TexCoord1  = input.TexCoord1;
    o.TexCoord2  = input.TexCoord2;
    o.Color      = input.Color;
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

// Clearcoat dielectric specular lobe (F0 = 0.04) times NdotL.
float ClearcoatSpecular(float3 Nv, float3 V, float3 L, float coatRough)
{
    float NdotL = dot(Nv, L);
    if (NdotL <= 0.0f)
        return 0.0f;
    float3 Hsum  = L + V;
    float  Hlen2 = dot(Hsum, Hsum);
    float3 H     = Hlen2 > 1e-8f ? Hsum * rsqrt(Hlen2) : Nv;
    float NdotV  = max(dot(Nv, V), 1e-4f);
    float NdotH  = saturate(dot(Nv, H));
    float VdotH  = saturate(dot(V, H));

    float a  = coatRough * coatRough;
    float a2 = a * a;
    float oneMinusNdotH2 = 1.0f - NdotH * NdotH;
    float d  = NdotH * NdotH * a2 + oneMinusNdotH2;
    float D  = a2 / max(PI * d * d, 1e-12f);

    float r  = coatRough + 1.0f;
    float k  = (r * r) / 8.0f;
    float gv = max(NdotV * (1.0f - k) + k, 1e-5f);
    float gl = max(NdotL * (1.0f - k) + k, 1e-5f);
    float Vis = 0.25f / (gv * gl);

    float F  = 0.04f + 0.96f * pow(1.0f - VdotH, 5.0f);
    return D * Vis * F * NdotL;
}

// ---- IBL helpers ----

float2 DirToEnvUV(float3 dir)
{
    return float2(atan2(dir.z, dir.x) * (0.5f / PI) + 0.5f,
                  0.5f - asin(clamp(dir.y, -1.0f, 1.0f)) / PI);
}

// Environment specular for reflection direction R at worldPos: probes whose box contains
// the pixel are sampled with a box-projected (parallax-corrected) direction and weighted by
// saturate(distance inside the box / blendDistance); weights above 1 are normalised and
// the remainder (1 - sum) comes from the global prefiltered map.
float3 SampleSpecularEnv(float3 R, float3 worldPos, float roughness)
{
    float envW, envH, envMips;
    g_prefilteredEnv.GetDimensions(0, envW, envH, envMips);
    float3 envColor = g_prefilteredEnv.SampleLevel(
        g_sampler, DirToEnvUV(R), roughness * (envMips - 1.0f)).rgb;

    int probeCount = (int)ProbeInfo.x;
    if (probeCount > 0)
    {
        float3 probeSum  = 0.0f;
        float  weightSum = 0.0f;
        float  probeLod  = roughness * (ProbeInfo.y - 1.0f);
        float3 safeR     = float3(abs(R.x) > 1e-6f ? R.x : 1e-6f,
                                  abs(R.y) > 1e-6f ? R.y : 1e-6f,
                                  abs(R.z) > 1e-6f ? R.z : 1e-6f);
        [loop] for (int p = 0; p < probeCount; ++p)
        {
            float3 center  = ProbeCenter[p].xyz;
            float3 extents = ProbeExtents[p].xyz;
            float3 inside  = extents - abs(worldPos - center);
            float  margin  = min(inside.x, min(inside.y, inside.z));
            if (margin <= 0.0f)
                continue;
            float w = saturate(margin / max(ProbeExtents[p].w, 1e-3f));
            // Ray from worldPos along R to the probe box, then the direction from its centre.
            float3 tA   = (center + extents - worldPos) / safeR;
            float3 tB   = (center - extents - worldPos) / safeR;
            float3 tFar = max(tA, tB);
            float  t    = min(tFar.x, min(tFar.y, tFar.z));
            float3 dir  = worldPos + R * t - center;
            probeSum  += w * g_reflectionProbes.SampleLevel(
                g_sampler, float3(DirToEnvUV(dir), (float)p), probeLod).rgb;
            weightSum += w;
        }
        if (weightSum > 1.0f)
        {
            probeSum /= weightSum;
            weightSum = 1.0f;
        }
        envColor = probeSum + (1.0f - weightSum) * envColor;
    }
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
    return envColor;
}

// IBL ambient:
//   Diffuse  — samples the panorama at max mip in the normal direction (approximate irradiance).
//   Specular — split-sum approximation: prefiltered env × BRDF(NdotV, roughness).
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
    float3 R        = reflect(-V, N);
    float3 envColor = SampleSpecularEnv(R, worldPos, roughness);

    float2 brdf = g_brdfLut.SampleLevel(
        g_sampler, float2(NdotV, 1.0f - roughness), 0).rg;

    float3 specular = envColor * (F0 * brdf.x + brdf.y) * IBLIntensity;

    return diffuse + specular;
}

float3 ClearcoatIBL(float3 Nv, float3 V, float3 worldPos, float coatRough)
{
    float  NdotV    = max(dot(Nv, V), 0.0f);
    float3 R        = reflect(-V, Nv);
    float3 envColor = SampleSpecularEnv(R, worldPos, coatRough);
    float2 brdf     = g_brdfLut.SampleLevel(
        g_sampler, float2(NdotV, 1.0f - coatRough), 0).rg;
    return envColor * (0.04f * brdf.x + brdf.y) * IBLIntensity;
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

// MaterialCB slot indices (SlotST / SlotUVA / SlotUVB order).
static const int SLOT_ALBEDO        = 0;
static const int SLOT_NORMAL        = 1;
static const int SLOT_ROUGHNESS     = 2;
static const int SLOT_EMISSIVE      = 3;
static const int SLOT_OCCLUSION     = 4;
static const int SLOT_DETAIL_ALBEDO = 5;
static const int SLOT_DETAIL_NORMAL = 6;

// Vertex UV set 0..2 (TEXCOORD0..2), or 3 = world planar (WorldPos.xz).
float2 SetUV(float set, PSIn i)
{
    return set > 2.5f ? i.WorldPos.xz
         : (set > 1.5f ? i.TexCoord2 : (set > 0.5f ? i.TexCoord1 : i.TexCoord));
}

// Texture coordinate of one material slot: its UV set, then uv * scale + offset.
float2 SlotUV(int slot, PSIn i)
{
    float set = (slot < 4 ? SlotUVA : SlotUVB)[slot & 3];
    return SetUV(set, i) * SlotST[slot].xy + SlotST[slot].zw;
}

// Sampling a tangent-space normal map (RG, reconstructed Z).
float3 SampleTangentNormal(Texture2D map, float2 uv)
{
    float2 rg = map.Sample(g_sampler, uv).rg * 2.0f - 1.0f;
    return float3(rg, sqrt(saturate(1.0f - dot(rg, rg))));
}

// Interleaved gradient noise in [0, 1) (Jimenez 2014), used to dither the
// cutout threshold so coverage resolves stably under TAA/FSR.
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

#if FOX_TERRAIN
// Layered terrain: layer i tiles world X/Z * TerrainScale[i]; the result starts at
// layer A and lerps toward B, C, D by the splat's R, G, B. Tangent-space layer normals
// blend with the same weights and are UDN-applied onto the vertex TBN. The overlay
// multiplies the colour: w = strength * overlay.a, rgb *= 1 - w + w * overlay.rgb.
void SampleTerrain(PSIn i, out float4 albedo, out float3 normalTS)
{
    float2 xz = i.WorldPos.xz;
    float2 uv = xz * TerrainScale.x;
    albedo    = g_terrainAlbedo0.Sample(g_sampler, uv);
    float3 n  = SampleTangentNormal(g_terrainNormal0, uv);
    if (TerrainParams.x > 1.5f)
    {
        float3 splat = g_terrainSplat.Sample(g_sampler, SetUV(TerrainParams2.x, i) * TerrainST.xy).rgb;
        uv     = xz * TerrainScale.y;
        albedo = lerp(albedo, g_terrainAlbedo1.Sample(g_sampler, uv), splat.r);
        n      = lerp(n, SampleTangentNormal(g_terrainNormal1, uv), splat.r);
        if (TerrainParams.x > 2.5f)
        {
            uv     = xz * TerrainScale.z;
            albedo = lerp(albedo, g_terrainAlbedo2.Sample(g_sampler, uv), splat.g);
            n      = lerp(n, SampleTangentNormal(g_terrainNormal2, uv), splat.g);
        }
        if (TerrainParams.x > 3.5f)
        {
            uv     = xz * TerrainScale.w;
            albedo = lerp(albedo, g_terrainAlbedo3.Sample(g_sampler, uv), splat.b);
            n      = lerp(n, SampleTangentNormal(g_terrainNormal3, uv), splat.b);
        }
    }
    if (TerrainParams.z > 0.0f)
    {
        float4 overlay = g_terrainOverlay.Sample(g_sampler, SetUV(TerrainParams.w, i) * TerrainST.zw);
        float  w       = TerrainParams.z * overlay.a;
        albedo.rgb    *= 1.0f - w + w * overlay.rgb;
    }
    normalTS = normalize(n);
}
#endif

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

    // Normal map → world-space N; the detail normal is UDN-blended in tangent space.
#if FOX_TERRAIN
    float4 albedo;
    float3 tbn_n;
    SampleTerrain(input, albedo, tbn_n);
    // Terrain layer alpha is roughness data (or unused), never opacity.
    float terrainRoughness = TerrainParams.y > 0.5f ? albedo.a : 1.0f;
    albedo.a = 1.0f;
#else
    float3 tbn_n  = SampleTangentNormal(g_normal, SlotUV(SLOT_NORMAL, input));
#endif
    if (SlotFlags.z > 0.5f)
    {
        float3 detailN = SampleTangentNormal(g_detailNormal, SlotUV(SLOT_DETAIL_NORMAL, input));
        tbn_n = normalize(float3(tbn_n.xy + detailN.xy * SlotUVB.w, tbn_n.z));
    }
    float3x3 TBN  = float3x3(normalize(input.T), normalize(input.B), normalize(input.N));
#if !FOX_TERRAIN
    // World-planar normal maps (UV set 3) align T with +world X (+u) and B with +world Z (+v).
    if (SlotUVA.y > 2.5f)
    {
        float3 N0 = normalize(input.N);
        float3 rawT = float3(1.0f, 0.0f, 0.0f) - N0 * N0.x;
        float  lenSqT = dot(rawT, rawT);
        if (lenSqT >= 1e-4f)
        {
            float3 T = rawT * rsqrt(lenSqT);
            float3 rawB = float3(0.0f, 0.0f, 1.0f) - N0 * N0.z;
            rawB -= T * dot(T, rawB);
            float lenSqB = dot(rawB, rawB);
            if (lenSqB >= 1e-4f)
                TBN = float3x3(T, rawB * rsqrt(lenSqB), N0);
        }
    }
#endif
    float3 N      = normalize(mul(tbn_n, TBN));

    float3 V        = normalize(CameraPos - input.WorldPos);
#if !FOX_TERRAIN
    float4 albedo   = g_albedo.Sample(g_sampler, SlotUV(SLOT_ALBEDO, input));
#endif
    float albedoSampleAlpha = albedo.a;
    // Vertex colour tint: 1 = multiply, 2 = albedo-alpha mask (alpha is mask, not opacity).
    if (TintParams.x > 1.5f)
    {
        albedo.rgb *= lerp(float3(1.0f, 1.0f, 1.0f), input.Color.rgb, albedo.a);
        albedo.a = 1.0f;
    }
    else if (TintParams.x > 0.5f)
    {
        albedo.rgb *= input.Color.rgb;
    }
    // Detail albedo (sRGB rgb, linear alpha):
    // 0 = overlay (scaled by TintParams.w = 1 / neutral), 1 = multiply, 2 = alphaBlend.
    // Albedo alpha is never changed by detail.
    if (SlotFlags.y > 0.5f)
    {
        float4 detail = g_detailAlbedo.Sample(g_sampler, SlotUV(SLOT_DETAIL_ALBEDO, input));
        float  s      = TintParams.z;
        if (TintParams.y > 1.5f)
            albedo.rgb = lerp(albedo.rgb, detail.rgb, saturate(detail.a * s));
        else if (TintParams.y > 0.5f)
            albedo.rgb *= lerp(float3(1.0f, 1.0f, 1.0f), detail.rgb, s);
        else
            albedo.rgb *= lerp(float3(1.0f, 1.0f, 1.0f), detail.rgb * TintParams.w, s);
    }
    albedo.rgb     *= AlbedoTint;

    // Alpha test (cutout): discard fragments below threshold. A temporal resolve
    // (TAA/FSR) lets the threshold move per pixel and frame, trading the hard
    // edge for stable dithered coverage instead of crawling holes.
    if (AlphaCutoff > 0.0f)
    {
        float ign = InterleavedGradientNoise(input.Position.xy + FrameIndex * 5.588238f);
        clip(albedo.a - (AlphaCutoff + (ign - 0.5f) * CutoutDitherWidth));
    }
    if (Clearcoat.x > 0.0f && Clearcoat.z > 0.5f)
        albedo.a = 1.0f;

    // Viewport overrides preserve alpha testing and never modify materials.
    // 2 = textured unlit, 3 = flat wireframe (rasterizer supplies triangle edges).
    if (Unlit > 1.5f)
    {
        float3 emission = g_emissive.Sample(g_sampler, SlotUV(SLOT_EMISSIVE, input)).rgb
                        * EmissiveColor * EmissiveIntensity;
        o.color = float4(Unlit > 2.5f ? float3(0.65f, 0.75f, 0.85f) : albedo.rgb + emission, albedo.a);
        o.normal = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return o;
    }

    float roughness;
    float metallic;
    float3 F0;
#if FOX_TERRAIN
    roughness = max(saturate(terrainRoughness * RoughnessScale), 0.04f);
    metallic  = 0.0f;
    F0        = float3(0.04f, 0.04f, 0.04f);
#else
    // See MaterialCB Workflow comment for the t1 texture interpretations.
    float2 roughnessUV = SlotUV(SLOT_ROUGHNESS, input);
    float4 mrSample = g_roughness.Sample(g_sampler, roughnessUV);
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
                                            : g_metallic.Sample(g_sampler, roughnessUV).r;
        metallic  = saturate(metalSample * Metallic);
        F0        = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallic);
    }
#endif
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

    // Clearcoat (KHR_materials_clearcoat): dielectric coat over the base PBR lobe using the
    // interpolated vertex normal. Branching on Clearcoat.x > 0 is uniform across the draw call.
    float3 Nv        = normalize(input.N);
    float  coat      = 0.0f;
    float  coatRough = 0.04f;
    float  baseAtten = 1.0f;
    if (Clearcoat.x > 0.0f)
    {
        uint  coatTexMask = (uint)(Clearcoat.w + 0.5f);
        float coatTex = 1.0f;
        if ((coatTexMask & 1u) != 0u)
        {
            float2 uv = SetUV(ClearcoatUV.x, input) * ClearcoatST[0].xy + ClearcoatST[0].zw;
            coatTex = g_clearcoat.Sample(g_sampler, uv).r;
        }
        float coatRoughTex = 1.0f;
        if ((coatTexMask & 2u) != 0u)
        {
            float2 uv = SetUV(ClearcoatUV.y, input) * ClearcoatST[1].xy + ClearcoatST[1].zw;
            coatRoughTex = g_clearcoatRoughness.Sample(g_sampler, uv).g;
        }
        coat      = saturate(Clearcoat.x * coatTex * (Clearcoat.z > 0.5f ? albedoSampleAlpha : 1.0f));
        coatRough = max(saturate(Clearcoat.y * coatRoughTex), 0.04f);

        float3 dnvdx = ddx(Nv);
        float3 dnvdy = ddy(Nv);
        float  coatVariance = dot(dnvdx, dnvdx) + dot(dnvdy, dnvdy);
        float  coatKernelRoughness = min(2.0f * coatVariance, 0.18f);
        coatRough = sqrt(saturate(coatRough * coatRough + coatKernelRoughness));

        float NdotVc = max(dot(Nv, V), 0.0f);
        float Fc     = F_Schlick(NdotVc, float3(0.04f, 0.04f, 0.04f)).x;
        baseAtten    = 1.0f - coat * Fc;
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

    // IBL ambient, darkened by baked occlusion (never the direct light terms; SSAO
    // multiplies the whole image afterwards, so the two combine by product).
    float ao = 1.0f;
    if (SlotFlags.x > 0.5f)
    {
        float2 occlusion = g_occlusion.Sample(g_sampler, SlotUV(SLOT_OCCLUSION, input)).rg;
        ao = lerp(1.0f, SlotFlags.w > 0.5f ? occlusion.g : occlusion.r, OcclusionStrength);
    }
    float3 ibl = IBL(N, V, input.WorldPos, albedo.rgb, roughness, metallic, F0);
    if (Clearcoat.x > 0.0f)
        ibl = ibl * baseAtten + coat * ClearcoatIBL(Nv, V, input.WorldPos, coatRough);
    float3 color = ibl * ao;

    // Directional light (Cook-Torrance)
    float3 sunBase = CookTorrance(N, V, L, albedo.rgb, roughness, metallic, F0);
    if (Clearcoat.x > 0.0f)
        color += shadow * LightColor * (sunBase * baseAtten + coat * ClearcoatSpecular(Nv, V, L, coatRough));
    else
        color += shadow * LightColor * sunBase;
    // Billboard foliage: wrapped (half-Lambert) diffuse stands in for sunlight
    // passing through the leaves, so cards facing away from the sun are not
    // black. Equal to Lambert when the card faces the sun; never below it.
    if (BillboardParams.x > 0.5f)
    {
        float wrap = saturate(dot(N, L) * 0.5f + 0.5f);
        color += shadow * LightColor * albedo.rgb * (1.0f - metallic) / PI * (wrap - NdotL);
    }

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
        float3 ptBase = CookTorrance(N, V, PL, albedo.rgb, roughness, metallic, F0);
        if (Clearcoat.x > 0.0f)
            color += ptShadow * atten * PointLights[i].Color
                   * (ptBase * baseAtten + coat * ClearcoatSpecular(Nv, V, PL, coatRough));
        else
            color += ptShadow * atten * PointLights[i].Color * ptBase;
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

                float3 spotBase = CookTorrance(N, V, SL, albedo.rgb, roughness, metallic, F0);
                if (Clearcoat.x > 0.0f)
                    color += spotShadow * cone * distAtten * SpotColor
                           * (spotBase * baseAtten + coat * ClearcoatSpecular(Nv, V, SL, coatRough));
                else
                    color += spotShadow * cone * distAtten * SpotColor * spotBase;
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
    float3 emissive = g_emissive.Sample(g_sampler, SlotUV(SLOT_EMISSIVE, input)).rgb;
    float emissiveSum = emissive.r + emissive.g + emissive.b;
    if (emissiveSum > 0.001f)
        color += emissive * EmissiveColor * EmissiveIntensity;

    // Store view-space normal and roughness for screen-space effects.
    float3 viewN = normalize(mul(N, (float3x3)View));

    o.color  = float4(color, albedo.a);
    o.normal = float4(viewN * 0.5f + 0.5f, roughness);
    return o;
}
