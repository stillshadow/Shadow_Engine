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
    // 外扩量定义在世界空间，再除以 Model 的平均轴向缩放，避免导入模型的
    // 100 倍展示缩放把轮廓同步放大成一圈厚壳。
    const float3 modelAxisLengths = float3(
        length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    const float averageModelScale = max(
        (modelAxisLengths.x + modelAxisLengths.y + modelAxisLengths.z) / 3.0F, 0.001F);
    const float localOutlineThickness = 0.01F / averageModelScale;
    const float3 expandedPosition = input.position +
        normalize(input.normal) * localOutlineThickness;
    output.position = mul(float4(expandedPosition, 1.0F), modelViewProjection);
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    return float4(1.0F, 0.55F, 0.05F, 1.0F);
}
