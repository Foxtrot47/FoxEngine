// Particle billboard shader with flipbook/atlas + soft particle support.
// Per-vertex: quad corner (POSITION) + UV (TEXCOORD0)
// Per-instance: world position + size (TEXCOORD1), color (TEXCOORD2), normalizedAge (TEXCOORD3)
// Camera-facing billboards constructed in the vertex shader.
// Atlas UVs computed from normalizedAge * atlasSpeed mapped to frame grid.
// Soft particles: fade alpha based on depth difference with scene geometry.

Texture2D    g_particleTex : register(t0);
Texture2D    g_depthTex    : register(t1);
SamplerState g_sampler     : register(s0);

cbuffer ParticleCameraCB : register(b0)
{
    row_major float4x4 ViewProj;
    float3 CamRight;
    float  _pad0;
    float3 CamUp;
    float  _pad1;
    // Atlas parameters
    float AtlasColumns;     // grid columns (1 = no atlas)
    float AtlasRows;        // grid rows
    float AtlasFrameCount;  // total frames
    float AtlasSpeed;       // playback speed (1 = full cycle over lifetime)
    // Soft particle parameters
    float NearZ;
    float FarZ;
    float SoftDistance;     // fade distance in world units
    float _pad2;
};

struct VSInput
{
    // Per-vertex (quad)
    float2 cornerPos : POSITION;
    float2 uv        : TEXCOORD0;
    // Per-instance
    float4 posSize   : TEXCOORD1;   // xyz = world pos, w = size
    float4 color     : TEXCOORD2;
    float  age       : TEXCOORD3;   // normalized age [0..1]
};

struct PSInput
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float2 uvNext    : TEXCOORD1;   // next frame UV for blending
    float  blend     : TEXCOORD2;   // lerp factor between frames
    float  linearZ   : TEXCOORD3;   // linear depth of this particle
    float4 color     : COLOR0;
};

PSInput VS_Main(VSInput input)
{
    PSInput o;

    float3 center = input.posSize.xyz;
    float  size   = input.posSize.w;

    // Expand quad corner into world space billboard
    float3 worldPos = center
                    + CamRight * (input.cornerPos.x * size)
                    + CamUp    * (input.cornerPos.y * size);

    o.pos   = mul(float4(worldPos, 1.0f), ViewProj);
    o.linearZ = o.pos.w;  // clip-space w = linear view-space depth for perspective projection
    o.color = input.color;

    // Compute atlas frame from normalized age
    float cols = AtlasColumns;
    float rows = AtlasRows;
    float frameCount = AtlasFrameCount;
    float speed = AtlasSpeed;

    // Guard against uninitialized CB (cols/rows = 0)
    if (cols < 1.0f) cols = 1.0f;
    if (rows < 1.0f) rows = 1.0f;
    if (frameCount < 1.0f) frameCount = cols * rows;

    float frameFloat = input.age * speed * (frameCount - 1.0f);
    frameFloat = clamp(frameFloat, 0.0f, frameCount - 1.0f);
    float frame0 = floor(frameFloat);
    float frame1 = min(frame0 + 1.0f, frameCount - 1.0f);
    o.blend = frameFloat - frame0;

    // Compute UV offset/scale for frame
    float invCols = 1.0f / cols;
    float invRows = 1.0f / rows;

    // Frame 0
    float col0 = fmod(frame0, cols);
    float row0 = floor(frame0 / cols);
    o.uv = float2((col0 + input.uv.x) * invCols,
                  (row0 + input.uv.y) * invRows);

    // Frame 1 (next frame for blending)
    float col1 = fmod(frame1, cols);
    float row1 = floor(frame1 / cols);
    o.uvNext = float2((col1 + input.uv.x) * invCols,
                      (row1 + input.uv.y) * invRows);

    return o;
}

float4 PS_Main(PSInput input) : SV_TARGET
{
    float4 tex0 = g_particleTex.Sample(g_sampler, input.uv);
    float4 tex1 = g_particleTex.Sample(g_sampler, input.uvNext);
    float4 tex  = lerp(tex0, tex1, input.blend);
    float4 result = tex * input.color;

    // Soft particle: fade based on depth difference
    if (SoftDistance > 0.0f)
    {
        // Read scene depth from depth buffer
        float2 screenUV = input.pos.xy;
        uint2 dims;
        g_depthTex.GetDimensions(dims.x, dims.y);
        screenUV /= float2(dims);

        float sceneDepthNDC = g_depthTex.Sample(g_sampler, screenUV).r;
        // Linearize scene depth (perspective projection)
        float sceneLinear = NearZ * FarZ / (FarZ - sceneDepthNDC * (FarZ - NearZ));
        float particleLinear = input.linearZ;

        float depthDiff = sceneLinear - particleLinear;
        float fade = saturate(depthDiff / SoftDistance);
        result.a *= fade;
    }

    return result;
}
