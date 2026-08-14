struct LightData
{
    float4 positionType;
    float4 directionIntensity;
    float4 colorRange;
    float4 areaSize;
};

// 为了让 Forward 与 Deferred 真正使用同一份 CPU 数据，这里保持 ObjectConstants 布局一致。
cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 modelViewProjection;
    float4 objectBaseColor;
    float3 cameraPosition;
    float objectRoughness;
    float3 legacyLightDirection;
    float iblIntensity;
    float3 legacyLightColor;
    float objectMetallic;
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

Texture2D<float4> gBufferBaseColorRoughness : register(t0);
Texture2D<float4> gBufferNormalMetallic : register(t1);
Texture2D<float4> gBufferWorldPosition : register(t2);
Texture2D<float> shadowMap : register(t3);
Texture2D<float> ssaoTexture : register(t4);
TextureCube<float4> environmentMap : register(t5);
TextureCube<float4> irradianceMap : register(t6);
TextureCube<float4> prefilteredEnvironmentMap : register(t7);
Texture2D<float2> brdfLut : register(t8);
TextureCube<float> pointShadowMap : register(t9);
SamplerState linearSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

static const float PI = 3.14159265359F;

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 positions[3] = {
        float2(-1.0F, -1.0F), float2(-1.0F, 3.0F), float2(3.0F, -1.0F)};
    const float2 position = positions[vertexId];
    output.position = float4(position, 0.0F, 1.0F);
    output.uv = float2(position.x * 0.5F + 0.5F, 0.5F - position.y * 0.5F);
    return output;
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    const float alpha = max(roughness * roughness, 0.0025F);
    const float alphaSquared = alpha * alpha;
    const float NdotH = saturate(dot(normal, halfVector));
    const float denominator = NdotH * NdotH * (alphaSquared - 1.0F) + 1.0F;
    return alphaSquared / max(PI * denominator * denominator, 0.0001F);
}

float GeometrySchlickGGX(float NdotDirection, float roughness)
{
    const float value = roughness + 1.0F;
    const float k = value * value / 8.0F;
    return NdotDirection / max(NdotDirection * (1.0F - k) + k, 0.0001F);
}

float GeometrySmith(float3 normal, float3 view, float3 light, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(normal, view)), roughness) *
        GeometrySchlickGGX(saturate(dot(normal, light)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0F - F0) * pow(1.0F - saturate(cosTheta), 5.0F);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max((1.0F - roughness).xxx, F0) - F0) *
        pow(1.0F - saturate(cosTheta), 5.0F);
}

float3 EvaluateDirectPbr(
    float3 normal, float3 view, float3 light, float3 baseColor,
    float metallic, float roughness, float3 radiance)
{
    const float NdotL = saturate(dot(normal, light));
    const float NdotV = saturate(dot(normal, view));
    const float3 halfVector = normalize(view + light);
    const float3 F0 = lerp(0.04F.xxx, baseColor, metallic);
    const float3 F = FresnelSchlick(saturate(dot(halfVector, view)), F0);
    const float3 specular = DistributionGGX(normal, halfVector, roughness) *
        GeometrySmith(normal, view, light, roughness) * F /
        max(4.0F * NdotV * NdotL, 0.0001F);
    const float3 diffuse = (1.0F - F) * (1.0F - metallic) * baseColor / PI;
    return (diffuse + specular) * radiance * NdotL;
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

void BuildBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    const float3 helper = abs(direction.y) < 0.95F
        ? float3(0.0F, 1.0F, 0.0F) : float3(1.0F, 0.0F, 0.0F);
    tangent = normalize(cross(helper, direction));
    bitangent = normalize(cross(direction, tangent));
}

float3 SampleEnvironment(float3 direction)
{
    return environmentMap.SampleLevel(
        linearSampler, normalize(RotateEnvironment(direction)), 0.0F).rgb *
        iblIntensity;
}

float3 SampleFilteredEnvironment(float3 direction, float roughness)
{
    direction = normalize(RotateEnvironment(direction));
    return prefilteredEnvironmentMap.SampleLevel(
        linearSampler, direction, roughness * 4.0F).rgb * iblIntensity;
}

float3 SampleDiffuseIrradiance(float3 normal)
{
    const float3 direction = normalize(RotateEnvironment(normal));
    return irradianceMap.SampleLevel(linearSampler, direction, 0.0F).rgb * iblIntensity;
}

float2 ApproximateEnvironmentBrdf(float NdotV, float roughness)
{
    return brdfLut.SampleLevel(
        linearSampler, float2(saturate(NdotV), saturate(roughness)), 0.0F);
}

float CalculateShadow(float3 worldPosition, float3 normal, float3 light)
{
    const float4 clipPosition = mul(float4(worldPosition, 1.0F), lightViewProjection);
    const float3 ndc = clipPosition.xyz / clipPosition.w;
    const float2 uv = float2(ndc.x * 0.5F + 0.5F, -ndc.y * 0.5F + 0.5F);
    if (ndc.z <= 0.0F || ndc.z >= 1.0F || any(uv < 0.0F) || any(uv > 1.0F))
    {
        return 1.0F;
    }
    uint width = 0;
    uint height = 0;
    shadowMap.GetDimensions(width, height);
    const float2 texelSize = 1.0F / float2(width, height);
    const float bias = max(0.0015F * (1.0F - dot(normal, light)), 0.00025F);
    float visibility = 0.0F;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += shadowMap.SampleCmpLevelZero(
                shadowSampler, uv + float2(x, y) * texelSize, ndc.z - bias);
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
            linearSampler, normalize(direction + offsets[sampleIndex]), 0.0F) *
            pointShadowRange;
        visibility += currentDistance - bias <= closestDistance ? 1.0F : 0.0F;
    }
    return visibility * 0.25F;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    const int2 pixel = int2(input.position.xy);
    const float4 worldPositionSample = gBufferWorldPosition.Load(int3(pixel, 0));
    clip(worldPositionSample.w - 0.5F); // 没有几何的像素保留先前绘制的天空。

    const float4 baseColorRoughness = gBufferBaseColorRoughness.Load(int3(pixel, 0));
    const float4 normalMetallic = gBufferNormalMetallic.Load(int3(pixel, 0));
    const float3 baseColor = baseColorRoughness.rgb;
    const float roughness = clamp(baseColorRoughness.a, 0.045F, 1.0F);
    const float metallic = saturate(normalMetallic.a);
    const float3 normal = normalize(normalMetallic.rgb * 2.0F - 1.0F);
    const float3 worldPosition = worldPositionSample.xyz;
    const float3 view = normalize(cameraPosition - worldPosition);

    if (debugViewMode > 0.5F && debugViewMode < 1.5F) return float4(baseColor, 1.0F);
    if (debugViewMode > 1.5F && debugViewMode < 2.5F) return float4(normal * 0.5F + 0.5F, 1.0F);
    if (debugViewMode > 2.5F && debugViewMode < 3.5F) return roughness.xxxx;
    if (debugViewMode > 3.5F && debugViewMode < 4.5F) return metallic.xxxx;
    if (debugViewMode > 7.5F && debugViewMode < 8.5F) return baseColorRoughness;
    if (debugViewMode > 8.5F && debugViewMode < 9.5F) return normalMetallic;
    if (debugViewMode > 9.5F && debugViewMode < 10.5F)
        return float4(saturate(abs(worldPosition) * 0.1F), 1.0F);
    if (debugViewMode > 10.5F && debugViewMode < 11.5F)
        return ssaoTexture.Load(int3(pixel, 0)).xxxx;

    float3 directLighting = 0.0F.xxx;
    float shadowFactor = 1.0F;
    [loop]
    for (uint lightIndex = 0; lightIndex < 8 && lightIndex < lightCount; ++lightIndex)
    {
        const LightData currentLight = lights[lightIndex];
        const uint type = (uint)currentLight.positionType.w;
        const float3 color = currentLight.colorRange.rgb;
        const float intensity = currentLight.directionIntensity.w;
        if (type == 0)
        {
            const float3 light = normalize(currentLight.directionIntensity.xyz);
            const float visibility = lightIndex == shadowLightIndex
                ? CalculateShadow(worldPosition, normal, light) : 1.0F;
            directLighting += EvaluateDirectPbr(
                normal, view, light, baseColor, metallic, roughness,
                color * intensity) * visibility;
            if (lightIndex == shadowLightIndex) shadowFactor = visibility;
        }
        else if (type == 1)
        {
            const float3 toLight = currentLight.positionType.xyz - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight < currentLight.colorRange.w)
            {
                const float3 light = toLight / max(distanceToLight, 0.001F);
                const float falloff = saturate(1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff / max(distanceToLight * distanceToLight, 1.0F);
                const float visibility = lightIndex == pointShadowLightIndex
                    ? CalculatePointShadow(
                        worldPosition, normal, light, currentLight.positionType.xyz)
                    : 1.0F;
                directLighting += EvaluateDirectPbr(
                    normal, view, light, baseColor, metallic, roughness,
                    color * intensity * attenuation) * visibility;
                if (lightIndex == pointShadowLightIndex) shadowFactor = visibility;
            }
        }
        else if (type == 2)
        {
            const float3 areaDirection = normalize(currentLight.directionIntensity.xyz);
            const float3 helper = abs(areaDirection.y) < 0.95F
                ? float3(0.0F, 1.0F, 0.0F) : float3(1.0F, 0.0F, 0.0F);
            const float3 right = normalize(cross(helper, areaDirection));
            const float3 up = normalize(cross(areaDirection, right));
            const float2 signs[4] = {
                float2(-0.5F, -0.5F), float2(0.5F, -0.5F),
                float2(-0.5F, 0.5F), float2(0.5F, 0.5F)};
            [unroll]
            for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
            {
                const float3 samplePosition = currentLight.positionType.xyz +
                    right * signs[sampleIndex].x * currentLight.areaSize.x +
                    up * signs[sampleIndex].y * currentLight.areaSize.y;
                const float3 toLight = samplePosition - worldPosition;
                const float distanceToLight = length(toLight);
                if (distanceToLight >= currentLight.colorRange.w) continue;
                const float3 light = toLight / max(distanceToLight, 0.001F);
                const float falloff = saturate(1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff / max(distanceToLight * distanceToLight, 1.0F);
                directLighting += EvaluateDirectPbr(
                    normal, view, light, baseColor, metallic, roughness,
                    color * intensity * attenuation * 0.25F);
            }
        }
        else
        {
            const float3 toLight = currentLight.positionType.xyz - worldPosition;
            const float distanceToLight = length(toLight);
            if (distanceToLight < currentLight.colorRange.w)
            {
                const float3 light = toLight / max(distanceToLight, 0.001F);
                const float coneCosine = dot(
                    normalize(currentLight.directionIntensity.xyz), -light);
                const float coneAttenuation = smoothstep(
                    currentLight.areaSize.y, currentLight.areaSize.x, coneCosine);
                const float falloff = saturate(
                    1.0F - distanceToLight / currentLight.colorRange.w);
                const float attenuation = falloff * falloff /
                    max(distanceToLight * distanceToLight, 1.0F);
                directLighting += EvaluateDirectPbr(
                    normal, view, light, baseColor, metallic, roughness,
                    color * intensity * attenuation * coneAttenuation);
            }
        }
    }

    const float NdotV = saturate(dot(normal, view));
    const float3 F0 = lerp(0.04F.xxx, baseColor, metallic);
    const float3 environmentFresnel = FresnelSchlickRoughness(NdotV, F0, roughness);
    const float3 diffuseWeight = (1.0F - environmentFresnel) * (1.0F - metallic);
    const float3 iblDiffuse = diffuseWeight * baseColor * SampleDiffuseIrradiance(normal);
    const float3 reflected = reflect(-view, normal);
    const float2 environmentBrdf = ApproximateEnvironmentBrdf(NdotV, roughness);
    const float3 iblSpecular = SampleFilteredEnvironment(reflected, roughness) *
        (environmentFresnel * environmentBrdf.x + environmentBrdf.y) *
        iblSpecularStrength;

    if (debugViewMode > 4.5F && debugViewMode < 5.5F) return float4(iblDiffuse, 1.0F);
    if (debugViewMode > 5.5F && debugViewMode < 6.5F) return float4(iblSpecular, 1.0F);
    if (debugViewMode > 6.5F && debugViewMode < 7.5F) return shadowFactor.xxxx;

    const float ao = ssaoTexture.Load(int3(pixel, 0));
    return float4(
        iblDiffuse * lerp(1.0F, ao, ssaoStrength) + iblSpecular + directLighting,
        1.0F);
}
