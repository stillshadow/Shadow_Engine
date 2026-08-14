cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4 unusedBaseColor;
    float3 pointLightPosition;
    float pointLightRange;
};

struct VertexInput
{
    float3 position : POSITION;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0F), modelViewProjection);
    output.worldPosition = mul(float4(input.position, 1.0F), model).xyz;
    return output;
}

float PSMain(VertexOutput input) : SV_TARGET
{
    // 六个面统一写入 0..1 的线性距离，主光照 Pass 可直接与像素到光源的距离比较。
    return saturate(length(input.worldPosition - pointLightPosition) /
        max(pointLightRange, 0.001F));
}
