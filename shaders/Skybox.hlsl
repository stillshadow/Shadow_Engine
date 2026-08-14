cbuffer SkyConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float3 cameraPosition;
    float intensity;
    float rotationRadians;
    float3 padding;
};

TextureCube<float4> environmentMap : register(t0);
SamplerState environmentSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 positions[3] = {
        float2(-1.0F, -1.0F),
        float2(-1.0F, 3.0F),
        float2(3.0F, -1.0F)};
    const float2 position = positions[vertexId];
    output.position = float4(position, 0.0F, 1.0F);
    output.uv = float2(position.x * 0.5F + 0.5F, 0.5F - position.y * 0.5F);
    return output;
}

float3 RotateAroundY(float3 direction, float radians)
{
    const float sine = sin(radians);
    const float cosine = cos(radians);
    return float3(
        direction.x * cosine - direction.z * sine,
        direction.y,
        direction.x * sine + direction.z * cosine);
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    // 从屏幕像素反推一条世界空间视线；天空因此只随相机旋转，不随相机平移。
    const float2 ndc = float2(input.uv.x * 2.0F - 1.0F, 1.0F - input.uv.y * 2.0F);
    const float4 world = mul(float4(ndc, 1.0F, 1.0F), inverseViewProjection);
    const float3 worldPosition = world.xyz / max(abs(world.w), 0.00001F);
    const float3 direction = RotateAroundY(
        normalize(worldPosition - cameraPosition), rotationRadians);
    return float4(environmentMap.SampleLevel(
        environmentSampler, direction, 0.0F).rgb * intensity, 1.0F);
}
