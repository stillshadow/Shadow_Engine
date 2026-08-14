cbuffer PostProcessConstants : register(b0)
{
    float exposure;
    float bloomThreshold;
    float bloomStrength;
    float bloomEnabled;
    float2 invResolution;
    float2 direction;
};

Texture2D<float4> hdrScene : register(t0);
Texture2D<float4> bloomTexture : register(t1);
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
    const float3 hdrColor = hdrScene.Sample(linearSampler, input.uv).rgb;
    const float3 bloomColor = bloomTexture.Sample(linearSampler, input.uv).rgb;
    const float3 exposed = max(hdrColor + bloomColor * bloomStrength, 0.0F.xxx) * exposure;
    const float3 mapped = exposed / (exposed + 1.0F.xxx);
    return float4(pow(mapped, 1.0F / 2.2F), 1.0F);
}
