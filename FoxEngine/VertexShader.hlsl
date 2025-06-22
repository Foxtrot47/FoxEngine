struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 color : Color;
};

PS_INPUT main(float2 pos : POSITION, float3 color : Color)
{
    PS_INPUT output;
    output.pos = float4(pos.x, pos.y, 0.0f, 1.0f); 
    output.color = color;
    return output;
}