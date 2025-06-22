float4 main(float4 pos: Position, float3 color : Color) : SV_TARGET
{
	return float4(color, 1.0f);
}