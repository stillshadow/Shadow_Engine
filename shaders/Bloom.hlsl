cbuffer PostProcessConstants : register(b0)
{
    float exposure;
    float bloomThreshold;
    float bloomStrength;
    float bloomEnabled;
    float2 invResolution;
    float2 direction;
};

Texture2D<float4> sourceTexture : register(t0);
SamplerState linearSampler : register(s0);

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0F, -1.0F), float2(-1.0F, 3.0F), float2(3.0F, -1.0F)};
    FullscreenOutput output;
    output.position = float4(positions[vertexId], 0.0F, 1.0F);
    output.uv = positions[vertexId] * float2(0.5F, -0.5F) + 0.5F;
    return output;
}

float4 PSMain(FullscreenOutput input) : SV_TARGET
{
    // direction 为零时执行亮部提取，否则执行一维五点高斯近似模糊。
    if (abs(direction.x) < 0.001F && abs(direction.y) < 0.001F)
    {
        const float3 color = sourceTexture.Sample(linearSampler, input.uv).rgb;
        const float luminance = dot(color, float3(0.2126F, 0.7152F, 0.0722F));
        const float mask = bloomEnabled > 0.5F
            ? smoothstep(bloomThreshold, bloomThreshold + 0.5F, luminance)
            : 0.0F;
        return float4(color * mask, 1.0F);
    }

    const float2 stepUv = direction * invResolution;
    const float3 c0 = sourceTexture.Sample(linearSampler, input.uv).rgb * 0.4026F;
    const float3 c1 = sourceTexture.Sample(linearSampler, input.uv + stepUv).rgb * 0.2442F;
    const float3 c2 = sourceTexture.Sample(linearSampler, input.uv - stepUv).rgb * 0.2442F;
    const float3 c3 = sourceTexture.Sample(linearSampler, input.uv + stepUv * 2.0F).rgb * 0.0545F;
    const float3 c4 = sourceTexture.Sample(linearSampler, input.uv - stepUv * 2.0F).rgb * 0.0545F;
    return float4(c0 + c1 + c2 + c3 + c4, 1.0F);
}
