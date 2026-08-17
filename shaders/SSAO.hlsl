cbuffer SsaoConstants : register(b0)
{
    float2 invResolution;
    float radius;
    float strength;
    float4x4 view;
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
    if (centerSample.w < 0.5F)
    {
        return 1.0F.xxxx;
    }

    const float3 centerNormal = normalize(
        gBufferNormalMetallic.Load(int3(pixel, 0)).xyz * 2.0F - 1.0F);
    const float centerViewDepth = mul(float4(centerSample.xyz, 1.0F), view).z;
    float occlusion = 0.0F;
    const int2 offsets[16] = {
        int2(-2, -2), int2(0, -2), int2(2, -2), int2(-2, 0),
        int2(2, 0), int2(-2, 2), int2(0, 2), int2(2, 2),
        int2(-5, 0), int2(5, 0), int2(0, -5), int2(0, 5),
        int2(-7, -7), int2(7, -7), int2(-7, 7), int2(7, 7)};
    [unroll]
    for (uint index = 0; index < 16; ++index)
    {
        const int2 samplePixel = pixel + offsets[index];
        const float4 samplePosition = gBufferWorldPosition.Load(int3(samplePixel, 0));
        if (samplePosition.w < 0.5F)
        {
            continue;
        }
        const float3 toSample = samplePosition.xyz - centerSample.xyz;
        const float distanceValue = length(toSample);
        if (distanceValue < 0.0001F || distanceValue >= radius)
        {
            continue;
        }
        const float facing = saturate(dot(centerNormal, toSample / distanceValue));
        // 当邻域表面比当前点更靠近相机时，它会遮挡当前点的环境光。
        const float sampleViewDepth = mul(float4(samplePosition.xyz, 1.0F), view).z;
        const float nearerToCamera = step(0.015F, centerViewDepth - sampleViewDepth);
        const float rangeWeight = 1.0F - smoothstep(0.0F, radius, distanceValue);
        occlusion += nearerToCamera * rangeWeight * (0.25F + 0.75F * facing);
    }
    // Strength 只在 Lighting Pass 混合一次，避免旧实现中强度被平方后视觉效果过弱。
    return saturate(1.0F - occlusion * (3.0F / 16.0F)).xxxx;
}
