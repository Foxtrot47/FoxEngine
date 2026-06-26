// FXAA 3.11 — Fast Approximate Anti-Aliasing
// Based on Timothy Lottes' FXAA 3.11 (public domain).
// Operates on LDR (post-tonemap) image. Uses luminance-based edge detection.

Texture2D    g_scene   : register(t0);
SamplerState g_sampler : register(s0);

cbuffer FXAACB : register(b0)
{
    float2 RcpFrame;   // 1.0 / screenSize
    float  SubpixQuality;  // 0.75 default (higher = more subpixel AA)
    float  EdgeThreshold;  // 0.166 default (lower = more edges detected)
    float  EdgeThresholdMin; // 0.0833 default (skip very dark edges)
    float3 _pad;
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

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299f, 0.587f, 0.114f));
}

float4 PS_Main(VSOutput input) : SV_TARGET
{
    float2 uv = input.uv;

    // Sample center and 4 neighbors
    float3 rgbM  = g_scene.Sample(g_sampler, uv).rgb;
    float3 rgbN  = g_scene.Sample(g_sampler, uv + float2( 0, -1) * RcpFrame).rgb;
    float3 rgbS  = g_scene.Sample(g_sampler, uv + float2( 0,  1) * RcpFrame).rgb;
    float3 rgbW  = g_scene.Sample(g_sampler, uv + float2(-1,  0) * RcpFrame).rgb;
    float3 rgbE  = g_scene.Sample(g_sampler, uv + float2( 1,  0) * RcpFrame).rgb;

    float lumaM = Luma(rgbM);
    float lumaN = Luma(rgbN);
    float lumaS = Luma(rgbS);
    float lumaW = Luma(rgbW);
    float lumaE = Luma(rgbE);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit if contrast too low
    if (lumaRange < max(EdgeThresholdMin, lumaMax * EdgeThreshold))
        return float4(rgbM, 1.0f);

    // Sample diagonal neighbors
    float3 rgbNW = g_scene.Sample(g_sampler, uv + float2(-1, -1) * RcpFrame).rgb;
    float3 rgbNE = g_scene.Sample(g_sampler, uv + float2( 1, -1) * RcpFrame).rgb;
    float3 rgbSW = g_scene.Sample(g_sampler, uv + float2(-1,  1) * RcpFrame).rgb;
    float3 rgbSE = g_scene.Sample(g_sampler, uv + float2( 1,  1) * RcpFrame).rgb;

    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);

    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaCornerSW_SE = lumaSW + lumaSE;
    float lumaCornerNW_NE = lumaNW + lumaNE;
    float lumaCornerNW_SW = lumaNW + lumaSW;
    float lumaCornerNE_SE = lumaNE + lumaSE;

    // Determine edge orientation (horizontal vs vertical)
    float edgeH = abs(-2.0f * lumaW + lumaCornerNW_SW) +
                  abs(-2.0f * lumaM + lumaNS) * 2.0f +
                  abs(-2.0f * lumaE + lumaCornerNE_SE);
    float edgeV = abs(-2.0f * lumaN + lumaCornerNW_NE) +
                  abs(-2.0f * lumaM + lumaWE) * 2.0f +
                  abs(-2.0f * lumaS + lumaCornerSW_SE);

    bool isHorizontal = (edgeH >= edgeV);

    // Choose step direction perpendicular to the edge
    float stepLength = isHorizontal ? RcpFrame.y : RcpFrame.x;
    float luma1 = isHorizontal ? lumaN : lumaW;
    float luma2 = isHorizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;

    bool is1Steeper = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25f * max(abs(gradient1), abs(gradient2));

    if (!is1Steeper) stepLength = -stepLength;

    // Subpixel aliasing test
    float lumaAvg = (1.0f / 12.0f) * (2.0f * (lumaNS + lumaWE) + lumaCornerNW_SW + lumaCornerNE_SE);
    float subpixOffset1 = saturate(abs(lumaAvg - lumaM) / lumaRange);
    float subpixOffset2 = (-2.0f * subpixOffset1 + 3.0f) * subpixOffset1 * subpixOffset1;
    float subpixOffset = subpixOffset2 * subpixOffset2 * SubpixQuality;

    // Start edge walking from center + half step
    float2 uvEdge = uv;
    if (isHorizontal) uvEdge.y += stepLength * 0.5f;
    else              uvEdge.x += stepLength * 0.5f;

    // Edge direction (along the edge)
    float2 edgeStep = isHorizontal ? float2(RcpFrame.x, 0.0f) : float2(0.0f, RcpFrame.y);

    // Walk along edge in both directions
    float lumaLocalAvg = 0.5f * (luma1 + luma2);

    float2 uvP = uvEdge + edgeStep;
    float2 uvN = uvEdge - edgeStep;
    float lumaEndP = Luma(g_scene.Sample(g_sampler, uvP).rgb) - lumaLocalAvg;
    float lumaEndN = Luma(g_scene.Sample(g_sampler, uvN).rgb) - lumaLocalAvg;

    bool reachedP = abs(lumaEndP) >= gradientScaled;
    bool reachedN = abs(lumaEndN) >= gradientScaled;

    // Up to 12 iterations along edge
    [unroll]
    for (int i = 2; i < 12; i++)
    {
        if (!reachedP)
        {
            uvP += edgeStep;
            lumaEndP = Luma(g_scene.Sample(g_sampler, uvP).rgb) - lumaLocalAvg;
            reachedP = abs(lumaEndP) >= gradientScaled;
        }
        if (!reachedN)
        {
            uvN -= edgeStep;
            lumaEndN = Luma(g_scene.Sample(g_sampler, uvN).rgb) - lumaLocalAvg;
            reachedN = abs(lumaEndN) >= gradientScaled;
        }
        if (reachedP && reachedN) break;
    }

    // Compute distance to edge endpoints
    float distP, distN;
    if (isHorizontal) { distP = uvP.x - uv.x; distN = uv.x - uvN.x; }
    else              { distP = uvP.y - uv.y; distN = uv.y - uvN.y; }

    float distMin = min(distP, distN);
    float edgeLength = distP + distN;
    float pixelOffset = -distMin / edgeLength + 0.5f;

    // Check if center is on the correct side
    bool isLumaMSmaller = lumaM < lumaLocalAvg;
    bool correctVariation = ((distP < distN ? lumaEndP : lumaEndN) < 0.0f) != isLumaMSmaller;

    float finalOffset = correctVariation ? pixelOffset : 0.0f;
    finalOffset = max(finalOffset, subpixOffset);

    // Apply offset
    float2 finalUV = uv;
    if (isHorizontal) finalUV.y += finalOffset * stepLength;
    else              finalUV.x += finalOffset * stepLength;

    return float4(g_scene.Sample(g_sampler, finalUV).rgb, 1.0f);
}
