// MotionBlur.hlsl — tile-based velocity motion blur (McGuire 2012 reconstruction filter).
// Select pass at compile time via MB_PASS:
//   0 = tile max velocity (16x16 tiles, velocity in target-pixel units)
//   1 = 3x3 neighbour max velocity
//   2 = depth-aware gather along neighbour-max velocity

#ifndef MB_PASS
#define MB_PASS 2
#endif

static const int   TILE_SIZE     = 16;
static const float SOFT_Z_EXTENT = 0.01f; // 1 cm in view-space depth

SamplerState g_point  : register(s0);
SamplerState g_linear : register(s1);

cbuffer MotionBlurCB : register(b0)
{
    row_major matrix InvViewProj;
    row_major matrix PrevViewProj;
    float2 TargetSize;
    float2 RcpTargetSize;
    float2 TileSize;
    float  ShutterScale;   // shutterAngle / 360
    float  MaxBlurPixels;
    float  NearZ;
    float  FarZ;
    int    SampleCount;
    float  _pad;
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

// Camera-only reprojection for sky pixels (depth == 1), matching TAA.hlsl.
float2 SkyVelocity(float2 uv)
{
    float2 ndc   = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 world = mul(float4(ndc, 1.0f, 1.0f), InvViewProj);
    float4 prev  = mul(world, PrevViewProj);
    if (prev.w <= 1e-4f)
        return float2(0.0f, 0.0f);
    float2 prevNDC = prev.xy / prev.w;
    return (ndc - prevNDC) * float2(0.5f, -0.5f);
}

// Scale UV-space velocity by shutter angle, convert to target pixels, and clamp.
float2 ScaleClampVelocityPx(float2 velUV)
{
    float2 vPx = velUV * ShutterScale * TargetSize;
    float  len = length(vPx);
    if (len > MaxBlurPixels)
        vPx *= (MaxBlurPixels / len);
    return vPx;
}

#if MB_PASS == 0

Texture2D g_velocity : register(t0);
Texture2D g_depth    : register(t1);

float2 ReadVelocityPx(float2 uv)
{
    float  depth = g_depth.SampleLevel(g_point, uv, 0).r;
    float2 velUV = (depth >= 0.99999f)
        ? SkyVelocity(uv)
        : g_velocity.SampleLevel(g_point, uv, 0).rg;
    return ScaleClampVelocityPx(velUV);
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    int2 tileOrigin = int2(input.pos.xy) * TILE_SIZE;
    int2 targetMax  = int2(TargetSize);
    int  xEnd       = min(TILE_SIZE, targetMax.x - tileOrigin.x);
    int  yEnd       = min(TILE_SIZE, targetMax.y - tileOrigin.y);

    float2 maxVel   = float2(0.0f, 0.0f);
    float  maxLenSq = 0.0f;

    for (int y = 0; y < yEnd; ++y)
    {
        for (int x = 0; x < xEnd; ++x)
        {
            float2 uv    = (float2(tileOrigin + int2(x, y)) + 0.5f) * RcpTargetSize;
            float2 v     = ReadVelocityPx(uv);
            float  lenSq = dot(v, v);
            if (lenSq > maxLenSq)
            {
                maxLenSq = lenSq;
                maxVel   = v;
            }
        }
    }

    return float4(maxVel, 0.0f, 0.0f);
}

#elif MB_PASS == 1

Texture2D g_tileMax : register(t0);

float4 PS_Main(VSOutput input) : SV_TARGET
{
    int2 tileCoord = int2(input.pos.xy);
    int2 tileLimit = int2(TileSize) - 1;

    float2 maxVel   = float2(0.0f, 0.0f);
    float  maxLenSq = 0.0f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2   c     = clamp(tileCoord + int2(dx, dy), int2(0, 0), tileLimit);
            float2 v     = g_tileMax.Load(int3(c, 0)).rg;
            float  lenSq = dot(v, v);
            if (lenSq > maxLenSq)
            {
                maxLenSq = lenSq;
                maxVel   = v;
            }
        }
    }

    return float4(maxVel, 0.0f, 0.0f);
}

#else // MB_PASS == 2

Texture2D g_scene       : register(t0);
Texture2D g_velocity    : register(t1);
Texture2D g_depth       : register(t2);
Texture2D g_neighborMax : register(t3);

float LinearDepth(float rawDepth)
{
    return (NearZ * FarZ) / max(FarZ - rawDepth * (FarZ - NearZ), 1e-6f);
}

float2 ReadVelocityPx(float2 uv, float rawDepth)
{
    float2 velUV = (rawDepth >= 0.99999f)
        ? SkyVelocity(uv)
        : g_velocity.SampleLevel(g_point, uv, 0).rg;
    return ScaleClampVelocityPx(velUV);
}

// McGuire 2012 soft depth compare in positive view-space depth (closer = smaller Z).
// Returns ~1 when zA is in front of zB, ~0 when behind by more than SOFT_Z_EXTENT.
float SoftDepthCompare(float zA, float zB)
{
    return saturate(1.0f - (zA - zB) / SOFT_Z_EXTENT);
}

float Cone(float distPx, float halfSpeedPx)
{
    return saturate(1.0f - distPx / max(halfSpeedPx, 1e-4f));
}

float Cylinder(float distPx, float halfSpeedPx)
{
    if (halfSpeedPx <= 1e-4f)
        return 0.0f;
    return 1.0f - smoothstep(0.95f * halfSpeedPx, 1.05f * halfSpeedPx, distPx);
}

float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv;
    float3 cX = g_scene.SampleLevel(g_point, uv, 0).rgb;

    int2   tileCoord = int2(min(uint2(input.pos.xy) / uint(TILE_SIZE), uint2(TileSize) - 1u));
    float2 vNeighbor = g_neighborMax.Load(int3(tileCoord, 0)).rg;
    float  speedN    = length(vNeighbor);
    if (speedN < 0.5f)
        return float4(cX, 1.0f);

    float  rawDepthX  = g_depth.SampleLevel(g_point, uv, 0).r;
    float  zX         = LinearDepth(rawDepthX);
    float2 vX         = ReadVelocityPx(uv, rawDepthX);
    float  halfSpeedX = 0.5f * length(vX);

    // Centre tap is always included; weight scales inversely with pixel blur radius
    // so moving foreground streaks can cover a fast-moving background.
    float  totalWeight = 1.0f / max(halfSpeedX, 0.5f);
    float3 sum         = cX * totalWeight;

    float jitter    = InterleavedGradientNoise(input.pos.xy);
    float2 uvMin    = RcpTargetSize * 0.5f;
    float2 uvMax    = 1.0f - uvMin;
    float  invCount = 1.0f / float(SampleCount);

    [loop]
    for (int i = 0; i < SampleCount; ++i)
    {
        float  t         = (float(i) + jitter) * invCount - 0.5f;
        float2 offsetPx  = vNeighbor * t;
        float2 sampleUV  = clamp(uv + offsetPx * RcpTargetSize, uvMin, uvMax);

        float  rawDepthY  = g_depth.SampleLevel(g_point, sampleUV, 0).r;
        float  zY         = LinearDepth(rawDepthY);
        float2 vY         = ReadVelocityPx(sampleUV, rawDepthY);
        float  halfSpeedY = 0.5f * length(vY);

        float distPx = abs(t) * speedN;
        float f      = SoftDepthCompare(zY, zX);
        float b      = SoftDepthCompare(zX, zY);

        float weight =
            f * Cone(distPx, halfSpeedY) +
            b * Cone(distPx, halfSpeedX) +
            2.0f * Cylinder(distPx, halfSpeedY) * Cylinder(distPx, halfSpeedX);

        float3 cY = g_scene.SampleLevel(g_linear, sampleUV, 0).rgb;
        sum         += cY * weight;
        totalWeight += weight;
    }

    return float4(sum / totalWeight, 1.0f);
}

#endif
