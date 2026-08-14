cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 modelViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 VSMain(VertexInput input) : SV_POSITION
{
    // Shadow Pass 不需要颜色、材质或法线，只把顶点变换到方向光的裁剪空间并写入深度。
    return mul(float4(input.position, 1.0F), modelViewProjection);
}
