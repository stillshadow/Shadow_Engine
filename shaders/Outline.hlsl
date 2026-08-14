// 选中物体的轮廓 Pass：沿顶点法线轻微外扩，再由模板测试只保留原物体之外的部分。
cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 modelViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    // 这是教学用的几何轮廓，外扩量保持很小，避免明显改变物体形状。
    const float3 expandedPosition = input.position + normalize(input.normal) * 0.025F;
    output.position = mul(float4(expandedPosition, 1.0F), modelViewProjection);
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    return float4(1.0F, 0.55F, 0.05F, 1.0F);
}
