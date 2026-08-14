#include "Assets/HdrEnvironment.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
constexpr float Pi = 3.14159265359F;

DirectX::XMFLOAT3 FaceDirection(
    const std::uint32_t face, const float u, const float v) noexcept
{
    using DirectX::XMFLOAT3;
    switch (face)
    {
    case 0: return XMFLOAT3{1.0F, -v, -u};
    case 1: return XMFLOAT3{-1.0F, -v, u};
    case 2: return XMFLOAT3{u, 1.0F, v};
    case 3: return XMFLOAT3{u, -1.0F, -v};
    case 4: return XMFLOAT3{u, -v, 1.0F};
    default: return XMFLOAT3{-u, -v, -1.0F};
    }
}

DirectX::XMFLOAT4 SampleEquirectangular(
    const float* pixels, const int width, const int height,
    const DirectX::XMFLOAT3& rawDirection) noexcept
{
    using namespace DirectX;
    const XMVECTOR normalized = XMVector3Normalize(XMLoadFloat3(&rawDirection));
    XMFLOAT3 direction{};
    XMStoreFloat3(&direction, normalized);

    const float longitude = std::atan2(direction.z, direction.x);
    const float latitude = std::asin(std::clamp(direction.y, -1.0F, 1.0F));
    const float sourceX = (longitude / (2.0F * Pi) + 0.5F) * width - 0.5F;
    const float sourceY = (0.5F - latitude / Pi) * height - 0.5F;
    const int x0Raw = static_cast<int>(std::floor(sourceX));
    const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)), 0, height - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const auto wrapX = [width](const int value)
    {
        const int wrapped = value % width;
        return wrapped < 0 ? wrapped + width : wrapped;
    };
    const int x0 = wrapX(x0Raw);
    const int x1 = wrapX(x0Raw + 1);
    const float tx = sourceX - std::floor(sourceX);
    const float ty = sourceY - std::floor(sourceY);
    const auto read = [&](const int x, const int y, const int channel)
    {
        return pixels[(static_cast<std::size_t>(y) * width + x) * 3 + channel];
    };

    XMFLOAT4 result{};
    float* output = &result.x;
    for (int channel = 0; channel < 3; ++channel)
    {
        const float top = std::lerp(read(x0, y0, channel), read(x1, y0, channel), tx);
        const float bottom = std::lerp(read(x0, y1, channel), read(x1, y1, channel), tx);
        output[channel] = std::lerp(top, bottom, ty);
    }
    result.w = 1.0F;
    return result;
}

float RadicalInverse(std::uint32_t bits) noexcept
{
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

DirectX::XMFLOAT2 Hammersley(
    const std::uint32_t index, const std::uint32_t count) noexcept
{
    return {static_cast<float>(index) / static_cast<float>(count), RadicalInverse(index)};
}

DirectX::XMVECTOR DirectionForTexel(
    const std::uint32_t face, const std::uint32_t x, const std::uint32_t y,
    const std::uint32_t size) noexcept
{
    const float u = 2.0F * (static_cast<float>(x) + 0.5F) / size - 1.0F;
    const float v = 2.0F * (static_cast<float>(y) + 0.5F) / size - 1.0F;
    const DirectX::XMFLOAT3 direction = FaceDirection(face, u, v);
    return DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&direction));
}

DirectX::XMFLOAT4 SampleCubemap(
    const Shadow::Assets::HdrCubemap& cubemap,
    const DirectX::XMVECTOR directionVector) noexcept
{
    using namespace DirectX;
    XMFLOAT3 direction{};
    XMStoreFloat3(&direction, XMVector3Normalize(directionVector));
    const float ax = std::abs(direction.x);
    const float ay = std::abs(direction.y);
    const float az = std::abs(direction.z);
    std::uint32_t face = 0;
    float u = 0.0F;
    float v = 0.0F;
    if (ax >= ay && ax >= az)
    {
        face = direction.x >= 0.0F ? 0U : 1U;
        u = direction.x >= 0.0F ? -direction.z / ax : direction.z / ax;
        v = -direction.y / ax;
    }
    else if (ay >= ax && ay >= az)
    {
        face = direction.y >= 0.0F ? 2U : 3U;
        u = direction.x / ay;
        v = direction.y >= 0.0F ? direction.z / ay : -direction.z / ay;
    }
    else
    {
        face = direction.z >= 0.0F ? 4U : 5U;
        u = direction.z >= 0.0F ? direction.x / az : -direction.x / az;
        v = -direction.y / az;
    }

    const float pixelX = (u * 0.5F + 0.5F) * cubemap.faceSize - 0.5F;
    const float pixelY = (v * 0.5F + 0.5F) * cubemap.faceSize - 0.5F;
    const int x0 = std::clamp(static_cast<int>(std::floor(pixelX)), 0,
        static_cast<int>(cubemap.faceSize) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(pixelY)), 0,
        static_cast<int>(cubemap.faceSize) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(cubemap.faceSize) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(cubemap.faceSize) - 1);
    const float tx = std::clamp(pixelX - std::floor(pixelX), 0.0F, 1.0F);
    const float ty = std::clamp(pixelY - std::floor(pixelY), 0.0F, 1.0F);
    const auto read = [&](const int x, const int y)
    {
        const std::size_t offset =
            (static_cast<std::size_t>(face) * cubemap.faceSize + y) *
                cubemap.faceSize + x;
        return XMLoadFloat4(&cubemap.rgba32f[offset]);
    };
    const XMVECTOR top = XMVectorLerp(read(x0, y0), read(x1, y0), tx);
    const XMVECTOR bottom = XMVectorLerp(read(x0, y1), read(x1, y1), tx);
    XMFLOAT4 result{};
    XMStoreFloat4(&result, XMVectorLerp(top, bottom, ty));
    return result;
}

void BuildBasis(
    const DirectX::XMVECTOR normal,
    DirectX::XMVECTOR& tangent,
    DirectX::XMVECTOR& bitangent) noexcept
{
    using namespace DirectX;
    const XMVECTOR helper = std::abs(XMVectorGetY(normal)) < 0.999F
        ? XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F)
        : XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
    tangent = XMVector3Normalize(XMVector3Cross(helper, normal));
    bitangent = XMVector3Cross(normal, tangent);
}

DirectX::XMVECTOR ToWorld(
    const DirectX::XMVECTOR local,
    const DirectX::XMVECTOR tangent,
    const DirectX::XMVECTOR bitangent,
    const DirectX::XMVECTOR normal) noexcept
{
    using namespace DirectX;
    return XMVector3Normalize(
        XMVectorScale(tangent, XMVectorGetX(local)) +
        XMVectorScale(bitangent, XMVectorGetY(local)) +
        XMVectorScale(normal, XMVectorGetZ(local)));
}

DirectX::XMVECTOR ImportanceSampleGgx(
    const DirectX::XMFLOAT2& xi,
    const DirectX::XMVECTOR normal,
    const float roughness) noexcept
{
    using namespace DirectX;
    const float alpha = roughness * roughness;
    const float phi = 2.0F * Pi * xi.x;
    const float cosineTheta = std::sqrt(
        (1.0F - xi.y) / (1.0F + (alpha * alpha - 1.0F) * xi.y));
    const float sineTheta = std::sqrt(std::max(1.0F - cosineTheta * cosineTheta, 0.0F));
    const XMVECTOR local = XMVectorSet(
        std::cos(phi) * sineTheta, std::sin(phi) * sineTheta, cosineTheta, 0.0F);
    XMVECTOR tangent{};
    XMVECTOR bitangent{};
    BuildBasis(normal, tangent, bitangent);
    return ToWorld(local, tangent, bitangent, normal);
}

float GeometrySchlickGgxIbl(const float ndot, const float roughness) noexcept
{
    const float k = roughness * roughness * 0.5F;
    return ndot / std::max(ndot * (1.0F - k) + k, 0.0001F);
}
} // namespace

namespace Shadow::Assets
{
HdrCubemap LoadHdrCubemap(
    const std::filesystem::path& path, const std::uint32_t faceSize)
{
    if (faceSize == 0)
    {
        throw std::invalid_argument("HDR cubemap face size must be greater than zero.");
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    float* source = stbi_loadf(path.string().c_str(), &width, &height, &channels, 3);
    if (source == nullptr || width <= 0 || height <= 0)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error(
            "Failed to load HDR environment: " + path.string() +
            (reason != nullptr ? " (" + std::string(reason) + ")" : ""));
    }

    HdrCubemap cubemap;
    cubemap.faceSize = faceSize;
    cubemap.rgba32f.resize(
        static_cast<std::size_t>(faceSize) * faceSize * 6);
    for (std::uint32_t face = 0; face < 6; ++face)
    {
        for (std::uint32_t y = 0; y < faceSize; ++y)
        {
            for (std::uint32_t x = 0; x < faceSize; ++x)
            {
                const float u =
                    (2.0F * (static_cast<float>(x) + 0.5F) / faceSize) - 1.0F;
                const float v =
                    (2.0F * (static_cast<float>(y) + 0.5F) / faceSize) - 1.0F;
                const std::size_t destination =
                    (static_cast<std::size_t>(face) * faceSize + y) * faceSize + x;
                cubemap.rgba32f[destination] = SampleEquirectangular(
                    source, width, height, FaceDirection(face, u, v));
            }
        }
    }
    stbi_image_free(source);
    return cubemap;
}

IblTextures BuildSplitSumIbl(
    const HdrCubemap& source,
    const std::uint32_t irradianceSize,
    const std::uint32_t prefilterSize,
    const std::uint32_t prefilterMipCount,
    const std::uint32_t brdfSize)
{
    using namespace DirectX;
    if (!source.IsValid() || irradianceSize == 0 || prefilterSize == 0 ||
        prefilterMipCount == 0 || brdfSize == 0)
    {
        throw std::invalid_argument("Invalid Split-Sum IBL generation settings.");
    }

    constexpr std::uint32_t sampleCount = 128;
    IblTextures output;
    output.irradiance.faceSize = irradianceSize;
    output.irradiance.rgba32f.resize(
        static_cast<std::size_t>(irradianceSize) * irradianceSize * 6);
    for (std::uint32_t face = 0; face < 6; ++face)
    {
        for (std::uint32_t y = 0; y < irradianceSize; ++y)
        {
            for (std::uint32_t x = 0; x < irradianceSize; ++x)
            {
                const XMVECTOR normal = DirectionForTexel(face, x, y, irradianceSize);
                XMVECTOR tangent{};
                XMVECTOR bitangent{};
                BuildBasis(normal, tangent, bitangent);
                XMVECTOR sum = XMVectorZero();
                for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
                {
                    const XMFLOAT2 xi = Hammersley(sample, sampleCount);
                    const float phi = 2.0F * Pi * xi.x;
                    const float cosineTheta = std::sqrt(1.0F - xi.y);
                    const float sineTheta = std::sqrt(xi.y);
                    const XMVECTOR local = XMVectorSet(
                        std::cos(phi) * sineTheta,
                        std::sin(phi) * sineTheta,
                        cosineTheta, 0.0F);
                    const XMVECTOR direction = ToWorld(
                        local, tangent, bitangent, normal);
                    const XMFLOAT4 radiance = SampleCubemap(source, direction);
                    sum += XMLoadFloat4(&radiance);
                }
                XMFLOAT4 irradiance{};
                XMStoreFloat4(
                    &irradiance,
                    XMVectorScale(sum, Pi / static_cast<float>(sampleCount)));
                irradiance.w = 1.0F;
                const std::size_t offset =
                    (static_cast<std::size_t>(face) * irradianceSize + y) *
                        irradianceSize + x;
                output.irradiance.rgba32f[offset] = irradiance;
            }
        }
    }

    output.prefilteredMips.reserve(prefilterMipCount);
    for (std::uint32_t mip = 0; mip < prefilterMipCount; ++mip)
    {
        HdrCubemap mipData;
        mipData.faceSize = std::max(prefilterSize >> mip, 1U);
        mipData.rgba32f.resize(
            static_cast<std::size_t>(mipData.faceSize) * mipData.faceSize * 6);
        const float roughness = prefilterMipCount == 1
            ? 0.0F
            : static_cast<float>(mip) / static_cast<float>(prefilterMipCount - 1);
        for (std::uint32_t face = 0; face < 6; ++face)
        {
            for (std::uint32_t y = 0; y < mipData.faceSize; ++y)
            {
                for (std::uint32_t x = 0; x < mipData.faceSize; ++x)
                {
                    const XMVECTOR normal = DirectionForTexel(
                        face, x, y, mipData.faceSize);
                    const XMVECTOR view = normal;
                    XMVECTOR sum = XMVectorZero();
                    float totalWeight = 0.0F;
                    for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
                    {
                        const XMVECTOR halfVector = ImportanceSampleGgx(
                            Hammersley(sample, sampleCount), normal,
                            std::max(roughness, 0.001F));
                        const XMVECTOR light = XMVector3Normalize(
                            XMVectorScale(
                                halfVector, 2.0F * XMVectorGetX(
                                    XMVector3Dot(view, halfVector))) - view);
                        const float ndotl = std::max(
                            XMVectorGetX(XMVector3Dot(normal, light)), 0.0F);
                        if (ndotl > 0.0F)
                        {
                            const XMFLOAT4 radiance = SampleCubemap(source, light);
                            sum += XMVectorScale(XMLoadFloat4(&radiance), ndotl);
                            totalWeight += ndotl;
                        }
                    }
                    XMFLOAT4 filtered{};
                    XMStoreFloat4(&filtered, XMVectorScale(
                        sum, 1.0F / std::max(totalWeight, 0.0001F)));
                    filtered.w = 1.0F;
                    const std::size_t offset =
                        (static_cast<std::size_t>(face) * mipData.faceSize + y) *
                            mipData.faceSize + x;
                    mipData.rgba32f[offset] = filtered;
                }
            }
        }
        output.prefilteredMips.push_back(std::move(mipData));
    }

    output.brdfSize = brdfSize;
    output.brdfLut.resize(static_cast<std::size_t>(brdfSize) * brdfSize);
    for (std::uint32_t y = 0; y < brdfSize; ++y)
    {
        const float roughness = (static_cast<float>(y) + 0.5F) / brdfSize;
        for (std::uint32_t x = 0; x < brdfSize; ++x)
        {
            const float ndotv = (static_cast<float>(x) + 0.5F) / brdfSize;
            const XMVECTOR normal = XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F);
            const XMVECTOR view = XMVectorSet(
                std::sqrt(std::max(1.0F - ndotv * ndotv, 0.0F)),
                0.0F, ndotv, 0.0F);
            float scale = 0.0F;
            float bias = 0.0F;
            for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
            {
                const XMVECTOR halfVector = ImportanceSampleGgx(
                    Hammersley(sample, sampleCount), normal, roughness);
                const XMVECTOR light = XMVector3Normalize(
                    XMVectorScale(
                        halfVector, 2.0F * XMVectorGetX(
                            XMVector3Dot(view, halfVector))) - view);
                const float ndotl = std::max(XMVectorGetZ(light), 0.0F);
                const float ndoth = std::max(XMVectorGetZ(halfVector), 0.0F);
                const float vdoth = std::max(
                    XMVectorGetX(XMVector3Dot(view, halfVector)), 0.0F);
                if (ndotl > 0.0F)
                {
                    const float geometry = GeometrySchlickGgxIbl(ndotv, roughness) *
                        GeometrySchlickGgxIbl(ndotl, roughness);
                    const float visibility = geometry * vdoth /
                        std::max(ndoth * ndotv, 0.0001F);
                    const float fresnel = std::pow(1.0F - vdoth, 5.0F);
                    scale += (1.0F - fresnel) * visibility;
                    bias += fresnel * visibility;
                }
            }
            output.brdfLut[static_cast<std::size_t>(y) * brdfSize + x] = {
                scale / sampleCount, bias / sampleCount};
        }
    }
    return output;
}
} // namespace Shadow::Assets
