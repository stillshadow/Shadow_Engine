struct ObjectConstants
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4 baseColor;
    float3 cameraPosition;
    float roughness;
    float3 emissiveFactor;
    float iblIntensity;
    float3 lightColor;
    float metallic;
    float debugViewMode;
    float environmentRotationRadians;
    float emissiveStrength;
    float normalStrength;
    float parallaxHeightScale;
    float ssaoStrength;
    float4x4 lightViewProjection;
    float4x4 normalMatrix;
};

cbuffer ObjectConstantsBuffer : register(b0)
{
    ObjectConstants objectConstants;
};

Texture2D<float4> baseColorTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
Texture2D<float4> metallicRoughnessTexture : register(t2);
Texture2D<float4> emissiveTexture : register(t15);
SamplerState materialSampler : register(s0);

struct InstanceData
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4x4 normalMatrix;
};
StructuredBuffer<InstanceData> instanceData : register(t14);

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
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 worldTangent : TEXCOORD3;
};

VertexOutput VSMain(VertexInput input, uint instanceId : SV_InstanceID)
{
    VertexOutput output;
    const InstanceData currentInstance = instanceData[instanceId];
    const float4 worldPosition = mul(float4(input.position, 1.0F), currentInstance.model);
    output.position = mul(float4(input.position, 1.0F), currentInstance.modelViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(
        mul(float4(input.normal, 0.0F), currentInstance.normalMatrix).xyz);
    output.uv = input.uv;
    float3 worldTangent = mul(
        float4(input.tangent.xyz, 0.0F), currentInstance.model).xyz;
    worldTangent = normalize(worldTangent - output.worldNormal *
        dot(worldTangent, output.worldNormal));
    output.worldTangent = float4(worldTangent, input.tangent.w);
    return output;
}

float2 ApplyParallax(float2 uv, float3 viewTangent)
{
    const float height = normalTexture.Sample(materialSampler, uv).r;
    const float safeViewZ = max(abs(viewTangent.z), 0.2F);
    return uv + (viewTangent.xy / safeViewZ) *
        ((height - 0.5F) * objectConstants.parallaxHeightScale);
}

struct GBufferOutput
{
    float4 baseColorRoughness : SV_TARGET0;
    float4 normalMetallic : SV_TARGET1;
    float4 worldPosition : SV_TARGET2;
    float4 emissive : SV_TARGET3;
};

GBufferOutput PSMain(VertexOutput input)
{
    const float3 view = normalize(objectConstants.cameraPosition - input.worldPosition);
    const float3 tangent = normalize(input.worldTangent.xyz);
    const float3 vertexNormal = normalize(input.worldNormal);
    const float3 bitangent = normalize(cross(vertexNormal, tangent)) * input.worldTangent.w;
    const float3 viewTangent = float3(
        dot(view, tangent), dot(view, bitangent), dot(view, vertexNormal));
    const float2 materialUv = ApplyParallax(input.uv, viewTangent);
    const float4 sampledBaseColor =
        baseColorTexture.Sample(materialSampler, materialUv) * objectConstants.baseColor;
    const float4 sampledMetallicRoughness =
        metallicRoughnessTexture.Sample(materialSampler, materialUv);
    const float roughness = saturate(
        objectConstants.roughness * sampledMetallicRoughness.g);
    const float metallic = saturate(
        objectConstants.metallic * sampledMetallicRoughness.b);

    float3 tangentNormal = normalTexture.Sample(materialSampler, materialUv).xyz * 2.0F - 1.0F;
    tangentNormal.xy *= objectConstants.normalStrength;
    tangentNormal = normalize(tangentNormal);
    const float3 normal = normalize(
        tangent * tangentNormal.x + bitangent * tangentNormal.y + vertexNormal * tangentNormal.z);

    GBufferOutput output;
    output.baseColorRoughness = float4(sampledBaseColor.rgb, roughness);
    output.normalMetallic = float4(normal * 0.5F + 0.5F, metallic);
    output.worldPosition = float4(input.worldPosition, 1.0F);
    output.emissive = float4(
        emissiveTexture.Sample(materialSampler, materialUv).rgb *
            objectConstants.emissiveFactor * objectConstants.emissiveStrength,
        1.0F);
    return output;
}
