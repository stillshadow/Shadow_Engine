cbuffer SsaoConstants : register(b0)
{
    float2 invResolution;
    float radius;
    float strength;
};

Texture2D<float4> gBufferNormalMetallic : register(t0);
Texture2D<float4> gBufferWorldPosition : register(t1);

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput VSMain(uint vertexId : SV_VertexID)
{
    // 一个三角形覆盖整个屏幕，避免四边形对角线带来的额外顶点。
    const float2 positions[3] = {
        float2(-1.0F, -1.0F), float2(-1.0F, 3.0F), float2(3.0F, -1.0F)};
    FullscreenOutput output;
    output.position = float4(positions[vertexId], 0.0F, 1.0F);
    output.uv = positions[vertexId] * float2(0.5F, -0.5F) + 0.5F;
    return output;
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
    const int2 pixel = int2(input.position.xy);
    const float4 centerSample = gBufferWorldPosition.Load(int3(pixel, 0));
    if (centerSample.w < 0.5F || strength <= 0.0001F)
    {
        return 1.0F.xxxx;
    }

    const float centerDepth = centerSample.z;
    const float3 centerNormal = normalize(
        gBufferNormalMetallic.Load(int3(pixel, 0)).xyz * 2.0F - 1.0F);
    float occlusion = 0.0F;
    const int2 offsets[8] = {
        int2(-2, -2), int2(0, -2), int2(2, -2), int2(-2, 0),
        int2(2, 0), int2(-2, 2), int2(0, 2), int2(2, 2)};
    [unroll]
    for (uint index = 0; index < 8; ++index)
    {
        const int2 samplePixel = pixel + offsets[index];
        const float4 samplePosition = gBufferWorldPosition.Load(int3(samplePixel, 0));
        const float3 toSample = samplePosition.xyz - centerSample.xyz;
        const float distanceValue = length(toSample);
        const float facing = saturate(dot(centerNormal, normalize(toSample)));
        // 当邻域表面比当前点更靠近相机时，它会遮挡当前点的环境光。
        occlusion += step(0.01F, centerDepth - samplePosition.z) *
            (1.0F - smoothstep(0.0F, radius, distanceValue)) * (0.35F + 0.65F * facing);
    }
    return saturate(1.0F - occlusion * 0.11F * strength).xxxx;
}
