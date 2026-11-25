// Billboard.hlsli — cylindrical billboard expansion shared by Basic.hlsl and the shadow depth shaders.
// A billboard vertex sits at its card's base point and carries the corner's (side, up) offset in
// metres in TEXCOORD2. The card turns about +Y so its front faces `toward`; only the horizontal
// part of `toward` is used.

struct BillboardFrame
{
    float3 f;      // horizontal unit direction the card faces
    float3 right;  // card right axis: +X when f = (0, 0, -1), the authored facing
};

BillboardFrame BillboardFacing(float3 toward)
{
    BillboardFrame frame;
    const float3 f = float3(toward.x, 0.0f, toward.z);
    const float len = length(f);
    frame.f     = len < 1e-4f ? float3(0.0f, 0.0f, -1.0f) : f / len;
    frame.right = float3(-frame.f.z, 0.0f, frame.f.x);
    return frame;
}

float3 BillboardExpand(float3 base, float3 right, float2 offset)
{
    return base + right * offset.x + float3(0.0f, 1.0f, 0.0f) * offset.y;
}
