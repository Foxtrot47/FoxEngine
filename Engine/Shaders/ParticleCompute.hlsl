// GPU Particle System — Compute shaders for emit and update.
// Simple pool-based approach: every slot is either alive (life > 0) or dead.
// Update dispatches over all slots; emit pops from dead list.

struct GPUParticle
{
    float3 position;
    float  life;
    float3 velocity;
    float  maxLife;
    float4 colorStart;
    float4 colorEnd;
    float  sizeStart;
    float  sizeEnd;
    float2 _pad;
};

// Resources
RWStructuredBuffer<GPUParticle> g_particles : register(u0);
RWByteAddressBuffer             g_counters  : register(u1); // [0]=deadCount, [4]=renderCount

// Counter offsets
#define COUNTER_DEAD   0
#define COUNTER_RENDER 4

// Instance output for rendering
struct InstanceOut
{
    float4 posAndSize;
    float4 color;
    float  normalizedAge;
    float3 _instPad;
};

RWStructuredBuffer<InstanceOut> g_instances : register(u2);
RWByteAddressBuffer             g_drawArgs  : register(u3); // indirect draw args

// Dead list — stores indices of available particle slots
RWStructuredBuffer<uint>        g_deadList  : register(u4);

// ------- EMIT SHADER -------
cbuffer EmitCB : register(b0)
{
    float3 EmitterPos;
    float  SpawnRadius;
    float3 VelocityMin;
    float  LifetimeMin;
    float3 VelocityMax;
    float  LifetimeMax;
    float4 ColorStart;
    float4 ColorEnd;
    float  SizeStart;
    float  SizeEnd;
    uint   EmitCount;
    float  RandomSeed;
};

float hash(float n) { return frac(sin(n) * 43758.5453123f); }

float3 randomDir(float seed)
{
    float u = hash(seed) * 2.0f - 1.0f;
    float v = hash(seed + 1.7f) * 6.28318f;
    float r = sqrt(1.0f - u * u);
    return float3(r * cos(v), u, r * sin(v));
}

float randomRange(float lo, float hi, float seed)
{
    return lo + (hi - lo) * hash(seed);
}

[numthreads(64, 1, 1)]
void CS_Emit(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= EmitCount)
        return;

    // Pop from dead list
    uint deadCount;
    g_counters.InterlockedAdd(COUNTER_DEAD, -1, deadCount);
    if ((int)deadCount <= 0)
    {
        g_counters.InterlockedAdd(COUNTER_DEAD, 1, deadCount);
        return;
    }

    uint particleIdx = g_deadList[deadCount - 1];

    float seed = RandomSeed + dtid.x * 3.14159f;

    GPUParticle p;
    float3 offset = randomDir(seed) * SpawnRadius * hash(seed + 0.3f);
    p.position   = EmitterPos + offset;
    p.velocity.x = randomRange(VelocityMin.x, VelocityMax.x, seed + 1.0f);
    p.velocity.y = randomRange(VelocityMin.y, VelocityMax.y, seed + 2.0f);
    p.velocity.z = randomRange(VelocityMin.z, VelocityMax.z, seed + 3.0f);
    p.maxLife    = randomRange(LifetimeMin, LifetimeMax, seed + 4.0f);
    p.life       = p.maxLife;
    p.colorStart = ColorStart;
    p.colorEnd   = ColorEnd;
    p.sizeStart  = SizeStart;
    p.sizeEnd    = SizeEnd;
    p._pad       = float2(0, 0);

    g_particles[particleIdx] = p;
}

// ------- UPDATE SHADER -------
cbuffer UpdateCB : register(b0)
{
    float  DeltaTime;
    float3 Gravity;
    uint   MaxParticles;
    float3 _updatePad;
};

[numthreads(256, 1, 1)]
void CS_Update(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= MaxParticles)
        return;

    GPUParticle p = g_particles[dtid.x];

    // Skip dead particles
    if (p.life <= 0.0f)
        return;

    // Update life
    p.life -= DeltaTime;

    if (p.life <= 0.0f)
    {
        // Kill: push to dead list
        p.life = 0.0f;
        g_particles[dtid.x] = p;

        uint deadIdx;
        g_counters.InterlockedAdd(COUNTER_DEAD, 1, deadIdx);
        g_deadList[deadIdx] = dtid.x;
        return;
    }

    // Integrate
    p.velocity += Gravity * DeltaTime;
    p.position += p.velocity * DeltaTime;
    g_particles[dtid.x] = p;

    // Write instance data for rendering
    float t = 1.0f - (p.life / p.maxLife);
    float size = lerp(p.sizeStart, p.sizeEnd, t);
    float4 color = lerp(p.colorStart, p.colorEnd, t);

    InstanceOut inst;
    inst.posAndSize = float4(p.position, size);
    inst.color = color;
    inst.normalizedAge = t;
    inst._instPad = float3(0, 0, 0);

    // Atomically get render slot
    uint renderIdx;
    g_drawArgs.InterlockedAdd(4, 1, renderIdx); // offset 4 = instanceCount
    g_instances[renderIdx] = inst;
}

// ------- RESET DRAW ARGS (called once per frame before update) -------
[numthreads(1, 1, 1)]
void CS_ResetArgs(uint3 dtid : SV_DispatchThreadID)
{
    // Reset indirect draw args: indexCount=6, instanceCount=0
    g_drawArgs.Store(0, 6);   // indexCountPerInstance
    g_drawArgs.Store(4, 0);   // instanceCount (filled by update)
    g_drawArgs.Store(8, 0);   // startIndexLocation
    g_drawArgs.Store(12, 0);  // baseVertexLocation
    g_drawArgs.Store(16, 0);  // startInstanceLocation
}
