// Geometry Shader 调试工具：每个输入三角形生成一条从中心指向平均法线方向的线。
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

struct VertexToGeometry
{
    float3 localPosition : POSITION0;
    float3 localNormal : NORMAL0;
};

struct GeometryToPixel
{
    float4 position : SV_POSITION;
};

VertexToGeometry VSMain(VertexInput input)
{
    VertexToGeometry output;
    output.localPosition = input.position;
    output.localNormal = input.normal;
    return output;
}

[maxvertexcount(2)]
void GSMain(triangle VertexToGeometry input[3], inout LineStream<GeometryToPixel> stream)
{
    const float3 center =
        (input[0].localPosition + input[1].localPosition + input[2].localPosition) / 3.0F;
    const float3 normal = normalize(
        input[0].localNormal + input[1].localNormal + input[2].localNormal);
    // 法线线段长度定义在世界空间，再除以 Model 的平均缩放，避免大型导入模型
    // 的调试线段被同步放大到遮住模型本身。
    const float3 modelAxisLengths = float3(
        length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
    const float averageModelScale = max(
        (modelAxisLengths.x + modelAxisLengths.y + modelAxisLengths.z) / 3.0F, 0.001F);
    const float localNormalLength = 0.12F / averageModelScale;

    GeometryToPixel start;
    start.position = mul(float4(center, 1.0F), modelViewProjection);
    stream.Append(start);

    GeometryToPixel end;
    end.position = mul(
        float4(center + normal * localNormalLength, 1.0F), modelViewProjection);
    stream.Append(end);
}

float4 PSMain(GeometryToPixel input) : SV_TARGET
{
    return float4(0.05F, 0.95F, 1.0F, 1.0F);
}
