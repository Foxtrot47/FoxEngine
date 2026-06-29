// GPU Particle billboard render shader.
// Reads instance data from a StructuredBuffer populated by compute shaders.

Texture2D    g_particleTex : register(t0);
Texture2D    g_depthTex    : register(t1);
SamplerState g_sampler     : register(s0);

struct InstanceData
{
    float4 posAndSize;
    float4 color;
    float  normalizedAge;
    float3 _pad;
};

StructuredBuffer<InstanceData> g_instances : register(t2);

cbuffer ParticleCameraCB : register(b0)
{
    row_major float4x4 ViewProj;
    float3 CamRight;
    float  _pad0;
    float3 CamUp;
    float  _pad1;
    float AtlasColumns;
    float AtlasRows;
    float AtlasFrameCount;
    float AtlasSpeed;
    float NearZ;
    float FarZ;
    float SoftDistance;
    float _pad2;
};

struct VSInput
{
    float2 cornerPos : POSITION;
    float2 uv        : TEXCOORD0;
    uint   instanceID : SV_InstanceID;
};

struct PSInput
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float2 uvNext    : TEXCOORD1;
    float  blend     : TEXCOORD2;
    float  linearZ   : TEXCOORD3;
    float4 color     : COLOR0;
};

PSInput VS_Main(VSInput input)
{
    PSInput o;

    InstanceData inst = g_instances[input.instanceID];
    float3 center = inst.posAndSize.xyz;
    float  size   = inst.posAndSize.w;

    float3 worldPos = center
                    + CamRight * (input.cornerPos.x * size)
                    + CamUp    * (input.cornerPos.y * size);

    o.pos     = mul(float4(worldPos, 1.0f), ViewProj);
    o.linearZ = o.pos.w;
    o.color   = inst.color;

    // Atlas UV calculation
    float cols = max(AtlasColumns, 1.0f);
    float rows = max(AtlasRows, 1.0f);
    float frameCount = AtlasFrameCount;
    if (frameCount < 1.0f) frameCount = cols * rows;
    float speed = AtlasSpeed;

    float frameFloat = inst.normalizedAge * speed * (frameCount - 1.0f);
    frameFloat = clamp(frameFloat, 0.0f, frameCount - 1.0f);
    float frame0 = floor(frameFloat);
    float frame1 = min(frame0 + 1.0f, frameCount - 1.0f);
    o.blend = frameFloat - frame0;

    float invCols = 1.0f / cols;
    float invRows = 1.0f / rows;

    float col0 = fmod(frame0, cols);
    float row0 = floor(frame0 / cols);
    o.uv = float2((col0 + input.uv.x) * invCols,
                  (row0 + input.uv.y) * invRows);

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

    if (SoftDistance > 0.0f)
    {
        float2 screenUV = input.pos.xy;
        uint2 dims;
        g_depthTex.GetDimensions(dims.x, dims.y);
        screenUV /= float2(dims);

        float sceneDepthNDC = g_depthTex.Sample(g_sampler, screenUV).r;
        float sceneLinear = NearZ * FarZ / (FarZ - sceneDepthNDC * (FarZ - NearZ));
        float particleLinear = input.linearZ;

        float depthDiff = sceneLinear - particleLinear;
        float fade = saturate(depthDiff / SoftDistance);
        result.a *= fade;
    }

    return result;
}
