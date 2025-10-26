
struct VertexPosColor
{
    float3 position : POSITION;
    float3 color    : COLOR;
};

struct VertexShaderOutput
{
	float4 color    : COLOR;
    float4 position : SV_Position;
};


[shader("vertex")]
VertexShaderOutput vertexMain(VertexPosColor posAndColor, uniform float4x4 mvp)
{
    VertexShaderOutput res;
    res.position = mul(mvp, float4(posAndColor.position, 1.0f));
    res.color = float4(posAndColor.color, 1.0f);
    return res;
}
