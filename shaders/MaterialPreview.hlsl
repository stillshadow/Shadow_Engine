struct LightData
{
    float4 positionType;
    float4 directionIntensity;
    float4 colorRange;
    float4 areaSize;
};

cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4 baseColor;
    float3 cameraPosition;
    float roughness;
    float3 lightDirection;
    float iblIntensity;
    float3 lightColor;
    float metallic;
    float debugViewMode;
    float environmentRotationRadians;
    float materialPadding;
    float normalStrength;
    float parallaxHeightScale;
    float ssaoStrength;
    float iblSpecularStrength;
    float renderPadding;
    float4x4 lightViewProjection;
    float4x4 normalMatrix;
    LightData lights[8];
    uint lightCount;
    uint shadowLightIndex;
    uint pointShadowLightIndex;
    float pointShadowRange;
};

Texture2D<float4> baseColorTexture : register(t0);
Texture2D<float4> normalTexture : register(t1);
Texture2D<float4> metallicRoughnessTexture : register(t2);
Texture2D<float> shadowMap : register(t3);
Texture2D<float4> gBufferBaseColorRoughness : register(t4);
Texture2D<float4> gBufferNormalMetallic : register(t5);
Texture2D<float4> gBufferWorldPosition : register(t6);
Texture2D<float> ssaoTexture : register(t8);
TextureCube<float4> environmentMap : register(t9);
TextureCube<float4> irradianceMap : register(t10);
TextureCube<float4> prefilteredEnvironmentMap : register(t11);
Texture2D<float2> brdfLut : register(t12);
TextureCube<float> pointShadowMap : register(t13);
struct InstanceData
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4x4 normalMatrix;
};
StructuredBuffer<InstanceData> instanceData : register(t14);
SamplerState materialSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

static const float PI = 3.14159265359F;

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

        const float3 worldNormal = normalize(
        mul(float4(input.normal, 0.0F), currentInstance.normalMatrix).xyz);
    output.worldNormal = worldNormal;
    output.uv = input.uv;

        float3 worldTangent = mul(
        float4(input.tangent.xyz, 0.0F), currentInstance.model).xyz;
    worldTangent = normalize(
        worldTangent - worldNormal * dot(worldTangent, worldNormal));
    output.worldTangent = float4(
        worldTangent, input.tangent.w);
    return output;
}

float DistributionGGX(float3 normal, float3 halfVector, float roughnessValue)
{
    const float alpha = max(roughnessValue * roughnessValue, 0.0025F);
    const float alphaSquared = alpha * alpha;
    const float NdotH = saturate(dot(normal, halfVector));
    const float denominatorTerm = NdotH * NdotH * (alphaSquared - 1.0F) + 1.0F;
    return alphaSquared / max(PI * denominatorTerm * denominatorTerm, 0.0001F);
}

float GeometrySchlickGGX(float NdotDirection, float roughnessValue)
{
    const float directLightingRoughness = roughnessValue + 1.0F;
    const float k = directLightingRoughness * directLightingRoughness / 8.0F;
    return NdotDirection / max(NdotDirection * (1.0F - k) + k, 0.0001F);
}

float GeometrySmith(float3 normal, float3 view, float3 light, float roughnessValue)
{
    const float NdotV = saturate(dot(normal, view));
    const float NdotL = saturate(dot(normal, light));
    return GeometrySchlickGGX(NdotV, roughnessValue) *
           GeometrySchlickGGX(NdotL, roughnessValue);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0F - F0) * pow(1.0F - saturate(cosTheta), 5.0F);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughnessValue)
{
    return F0 + (max((1.0F - roughnessValue).xxx, F0) - F0) *
        pow(1.0F - saturate(cosTheta), 5.0F);
}

float3 RotateEnvironment(float3 direction)
{
    const float sine = sin(environmentRotationRadians);
    const float cosine = cos(environmentRotationRadians);
    return float3(
        direction.x * cosine - direction.z * sine,
        direction.y,
        direction.x * sine + direction.z * cosine);
}

void BuildDirectionBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    const float3 helper = abs(direction.y) < 0.95F
        ? float3(0.0F, 1.0F, 0.0F)
        : float3(1.0F, 0.0F, 0.0F);
    tangent = normalize(cross(helper, direction));
    bitangent = normalize(cross(direction, tangent));
}

float3 SampleEnvironment(float3 sampleDirection)
{
    const float3 direction = normalize(RotateEnvironment(sampleDirection));
    return environmentMap.SampleLevel(materialSampler, direction, 0.0F).rgb *
        iblIntensity;
}

float3 SampleFilteredEnvironment(float3 sampleDirection, float roughnessValue)
{
    const float3 direction = normalize(RotateEnvironment(sampleDirection));
    return prefilteredEnvironmentMap.SampleLevel(
        materialSampler, direction, roughnessValue * 4.0F).rgb * iblIntensity;
}

float3 SampleDiffuseIrradiance(float3 surfaceNormal)
{
    const float3 direction = normalize(RotateEnvironment(surfaceNormal));
    return irradianceMap.SampleLevel(materialSampler, direction, 0.0F).rgb *
        iblIntensity;
}

float2 ApproximateEnvironmentBrdf(float NdotV, float roughnessValue)
{
    return brdfLut.SampleLevel(
        materialSampler, float2(saturate(NdotV), saturate(roughnessValue)), 0.0F);
}

float CalculateDirectionalShadow(float3 worldPosition, float3 normal, float3 light)
{
    const float4 lightClipPosition =
        mul(float4(worldPosition, 1.0F), lightViewProjection);
    const float3 lightNdc = lightClipPosition.xyz / lightClipPosition.w;
    const float2 shadowUv = float2(
        lightNdc.x * 0.5F + 0.5F,
        -lightNdc.y * 0.5F + 0.5F);
    if (lightNdc.z <= 0.0F || lightNdc.z >= 1.0F ||
        any(shadowUv < 0.0F) || any(shadowUv > 1.0F))
    {
        return 1.0F;
    }

    uint shadowWidth = 0;
    uint shadowHeight = 0;
    shadowMap.GetDimensions(shadowWidth, shadowHeight);
    const float2 texelSize = 1.0F / float2(shadowWidth, shadowHeight);
    const float receiverBias = max(0.0015F * (1.0F - dot(normal, light)), 0.00025F);
    float visibility = 0.0F;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += shadowMap.SampleCmpLevelZero(
                shadowSampler, shadowUv + float2(x, y) * texelSize,
                lightNdc.z - receiverBias);
        }
    }
    return visibility / 9.0F;
}

float CalculatePointShadow(
    float3 worldPosition, float3 normal, float3 light, float3 lightPosition)
{
    const float3 fromLight = worldPosition - lightPosition;
    const float currentDistance = length(fromLight);
    const float bias = max(0.015F * (1.0F - dot(normal, light)), 0.004F);
    const float3 direction = fromLight / max(currentDistance, 0.001F);
    const float3 offsets[4] = {
        float3(0.012F, 0.0F, 0.0F), float3(-0.012F, 0.0F, 0.0F),
        float3(0.0F, 0.012F, 0.0F), float3(0.0F, -0.012F, 0.0F)};
    float visibility = 0.0F;
    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        const float closestDistance = pointShadowMap.SampleLevel(
            materialSampler, normalize(direction + offsets[sampleIndex]), 0.0F) *
            pointShadowRange;
        visibility += currentDistance - bias <= closestDistance ? 1.0F : 0.0F;
    }
    return visibility * 0.25F;
}

float2 ApplyParallax(float2 uv, float3 viewTangent)
{
    // 用法线贴图的红色通道复用一个轻量高度值；高度为 0 时完全退化为普通 UV。
    const float height = normalTexture.Sample(materialSampler, uv).r;
    const float safeViewZ = max(abs(viewTangent.z), 0.2F);
    return uv + (viewTangent.xy / safeViewZ) * ((height - 0.5F) * parallaxHeightScale);
}

float3 EvaluateDirectPbr(
    float3 normal,
    float3 view,
    float3 light,
    float3 surfaceBaseColorValue,
    float metallicValue,
    float roughnessValue,
    float3 radiance)
{
    const float NdotL = saturate(dot(normal, light));
    const float NdotV = saturate(dot(normal, view));
    const float3 halfVector = normalize(view + light);
    const float3 F0 = lerp(0.04F.xxx, surfaceBaseColorValue, metallicValue);
    const float D = DistributionGGX(normal, halfVector, roughnessValue);
    const float G = GeometrySmith(normal, view, light, roughnessValue);
    const float3 F = FresnelSchlick(saturate(dot(halfVector, view)), F0);
    const float3 specular = D * G * F /
        max(4.0F * NdotV * NdotL, 0.0001F);
    const float3 kD = (1.0F - F) * (1.0F - metallicValue);
    const float3 diffuse = kD * surfaceBaseColorValue / PI;
    return (diffuse + specular) * radiance * NdotL;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    const float3 view = normalize(cameraPosition - input.worldPosition);
    const float3 tangent = normalize(input.worldTangent.xyz);
    const float3 viewBitangent = normalize(cross(normalize(input.worldNormal), tangent)) * input.worldTangent.w;
    const float3 viewTangent = float3(dot(view, tangent), dot(view, viewBitangent), dot(view, normalize(input.worldNormal)));
    const float2 materialUv = ApplyParallax(input.uv, viewTangent);
    const float4 sampledBaseColor = baseColorTexture.Sample(materialSampler, materialUv) * baseColor;
    const float4 sampledMetallicRoughness = metallicRoughnessTexture.Sample(materialSampler, materialUv);
    const float clampedRoughness = clamp(
        roughness * sampledMetallicRoughness.g, 0.045F, 1.0F);
    const float clampedMetallic = saturate(metallic * sampledMetallicRoughness.b);

    const float3 vertexNormal = normalize(input.worldNormal);
    const float3 bitangent = normalize(cross(vertexNormal, tangent)) * input.worldTangent.w;
    float3 tangentNormal = normalTexture.Sample(materialSampler, materialUv).xyz * 2.0F - 1.0F;
    tangentNormal.xy *= normalStrength;
    tangentNormal = normalize(tangentNormal);
    const float3 normal = normalize(
        tangent * tangentNormal.x + bitangent * tangentNormal.y + vertexNormal * tangentNormal.z);
    const float3 surfaceBaseColor = sampledBaseColor.rgb;
    const float surfaceRoughness = clampedRoughness;

    if (debugViewMode > 0.5F && debugViewMode < 1.5F)
    {
        return float4(surfaceBaseColor, 1.0F);
    }
    if (debugViewMode > 1.5F && debugViewMode < 2.5F)
    {
        return float4(normal * 0.5F + 0.5F, 1.0F);
    }
    if (debugViewMode > 2.5F && debugViewMode < 3.5F)
    {
        return float4(surfaceRoughness.xxx, 1.0F);
    }
    if (debugViewMode > 3.5F && debugViewMode < 4.5F)
    {
        return float4(clampedMetallic.xxx, 1.0F);
    }
    if (debugViewMode > 7.5F && debugViewMode < 8.5F)
    {
        return gBufferBaseColorRoughness.Load(int3(int2(input.position.xy), 0));
    }
    if (debugViewMode > 8.5F && debugViewMode < 9.5F)
    {
        return gBufferNormalMetallic.Load(int3(int2(input.position.xy), 0));
    }
    if (debugViewMode > 9.5F && debugViewMode < 10.5F)
    {
        const float3 position = gBufferWorldPosition.Load(
            int3(int2(input.position.xy), 0)).xyz;
        return float4(saturate(abs(position) * 0.1F), 1.0F);
    }
    if (debugViewMode > 10.5F && debugViewMode < 11.5F)
    {
        return ssaoTexture.Load(int3(int2(input.position.xy), 0)).xxxx;
    }
    float3 directLighting = 0.0F.xxx;
    float shadowDebugValue = 1.0F;

    [loop]
    for (uint lightIndex = 0; lightIndex < 8; ++lightIndex)
    {
        if (lightIndex >= lightCount)
        {
            break;
        }

        const LightData currentLight = lights[lightIndex];
        const uint currentType = (uint)currentLight.positionType.w;
        const float3 currentColor = currentLight.colorRange.rgb;
        const float currentIntensity = currentLight.directionIntensity.w;
        if (currentType == 0)
        {
            const float3 light = normalize(currentLight.directionIntensity.xyz);
            const float visibility = lightIndex == shadowLightIndex
                ? CalculateDirectionalShadow(input.worldPosition, normal, light)
                : 1.0F;
            directLighting += EvaluateDirectPbr(
                normal, view, light, surfaceBaseColor, clampedMetallic,
                surfaceRoughness, currentColor * currentIntensity) * visibility;
            if (lightIndex == shadowLightIndex)
            {
                shadowDebugValue = visibility;
            }
        }
        else if (currentType == 1)
        {
            const float3 toLight = currentLight.positionType.xyz - input.worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight < currentLight.colorRange.w)
            {
                const float3 light = toLight / max(distanceToLight, 0.001F);
                const float falloff = saturate(
                    1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff /
                    max(distanceToLight * distanceToLight, 1.0F);
                const float visibility = lightIndex == pointShadowLightIndex
                    ? CalculatePointShadow(
                        input.worldPosition, normal, light,
                        currentLight.positionType.xyz)
                    : 1.0F;
                directLighting += EvaluateDirectPbr(
                    normal, view, light, surfaceBaseColor, clampedMetallic,
                    surfaceRoughness,
                    currentColor * currentIntensity * attenuation) * visibility;
                if (lightIndex == pointShadowLightIndex)
                {
                    shadowDebugValue = visibility;
                }
            }
        }
        else if (currentType == 2)
        {
            const float3 areaDirection = normalize(currentLight.directionIntensity.xyz);
            const float3 helperAxis = abs(areaDirection.y) < 0.95F
                ? float3(0.0F, 1.0F, 0.0F)
                : float3(1.0F, 0.0F, 0.0F);
            const float3 areaRight = normalize(cross(helperAxis, areaDirection));
            const float3 areaUp = normalize(cross(areaDirection, areaRight));
            const float2 sampleSigns[4] = {
                float2(-0.5F, -0.5F), float2(0.5F, -0.5F),
                float2(-0.5F, 0.5F), float2(0.5F, 0.5F)};
            [unroll]
            for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
            {
                const float3 samplePosition = currentLight.positionType.xyz +
                    areaRight * sampleSigns[sampleIndex].x * currentLight.areaSize.x +
                    areaUp * sampleSigns[sampleIndex].y * currentLight.areaSize.y;
                const float3 toLight = samplePosition - input.worldPosition;
                const float distanceToLight = length(toLight);
                if (distanceToLight >= currentLight.colorRange.w)
                {
                    continue;
                }
                const float3 light = toLight / max(distanceToLight, 0.001F);
                const float falloff = saturate(
                    1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff /
                    max(distanceToLight * distanceToLight, 1.0F);
                directLighting += EvaluateDirectPbr(
                    normal, view, light, surfaceBaseColor, clampedMetallic,
                    surfaceRoughness,
                    currentColor * currentIntensity * attenuation * 0.25F);
            }
        }
        else
        {
            const float3 toLight = currentLight.positionType.xyz - input.worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight < currentLight.colorRange.w)
            {
                const float3 light = toLight / max(distanceToLight, 0.001F);
                // directionIntensity.xyz 表示灯光向外发射的方向；从灯到像素的方向是 -light。
                const float coneCosine = dot(
                    normalize(currentLight.directionIntensity.xyz), -light);
                const float coneAttenuation = smoothstep(
                    currentLight.areaSize.y, currentLight.areaSize.x, coneCosine);
                const float falloff = saturate(
                    1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff /
                    max(distanceToLight * distanceToLight, 1.0F);
                directLighting += EvaluateDirectPbr(
                    normal, view, light, surfaceBaseColor, clampedMetallic,
                    surfaceRoughness,
                    currentColor * currentIntensity * attenuation * coneAttenuation);
            }
        }
    }

    const float NdotV = saturate(dot(normal, view));
    const float3 F0 = lerp(0.04F.xxx, surfaceBaseColor, clampedMetallic);
    const float3 environmentFresnel =
        FresnelSchlickRoughness(NdotV, F0, surfaceRoughness);
    const float3 environmentDiffuseWeight =
        (1.0F - environmentFresnel) * (1.0F - clampedMetallic);
    const float3 irradiance = SampleDiffuseIrradiance(normal);
    const float3 iblDiffuse =
        environmentDiffuseWeight * surfaceBaseColor * irradiance;
    const float3 reflection = reflect(-view, normal);
    const float3 prefilteredRadiance =
        SampleFilteredEnvironment(reflection, surfaceRoughness);
    const float2 environmentBrdf = ApproximateEnvironmentBrdf(NdotV, surfaceRoughness);
    const float3 iblSpecular =
        prefilteredRadiance *
        (environmentFresnel * environmentBrdf.x + environmentBrdf.y) *
        iblSpecularStrength;

    if (debugViewMode > 4.5F && debugViewMode < 5.5F)
    {
        return float4(iblDiffuse, 1.0F);
    }
    if (debugViewMode > 5.5F && debugViewMode < 6.5F)
    {
        return float4(iblSpecular, 1.0F);
    }
    if (debugViewMode > 6.5F && debugViewMode < 7.5F)
    {
        return float4(shadowDebugValue.xxx, 1.0F);
    }

    float ambientOcclusion = 1.0F;
    if (ssaoStrength > 0.0001F)
    {
        ambientOcclusion = ssaoTexture.Load(int3(int2(input.position.xy), 0));
    }
    // 保持线性 HDR；曝光、Tone Mapping 和 Gamma 只在最终后处理执行一次。
    const float3 finalColor =
        iblDiffuse * lerp(1.0F, ambientOcclusion, ssaoStrength) +
        iblSpecular + directLighting;
    return float4(finalColor, sampledBaseColor.a);
}
