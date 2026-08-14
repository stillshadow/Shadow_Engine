#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Shadow::Assets
{
struct HdrCubemap
{
    std::uint32_t faceSize = 0;
    // 顺序遵循 D3D12 TextureCube：+X、-X、+Y、-Y、+Z、-Z。
    std::vector<DirectX::XMFLOAT4> rgba32f;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return faceSize > 0 && rgba32f.size() ==
            static_cast<std::size_t>(faceSize) * faceSize * 6;
    }
};

struct IblTextures
{
    HdrCubemap irradiance;
    // 每项是一层完整 Cubemap，尺寸从大到小；Shader 用 Roughness 选择 Mip。
    std::vector<HdrCubemap> prefilteredMips;
    std::uint32_t brdfSize = 0;
    std::vector<DirectX::XMFLOAT2> brdfLut;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return irradiance.IsValid() && !prefilteredMips.empty() &&
            brdfSize > 0 && brdfLut.size() ==
                static_cast<std::size_t>(brdfSize) * brdfSize;
    }
};

// 把 LearnOpenGL 使用的经纬度 HDR 全景图在 CPU 上转换为真正的六面 Cubemap。
[[nodiscard]] HdrCubemap LoadHdrCubemap(
    const std::filesystem::path& path, std::uint32_t faceSize);

// 在 CPU 上生成 LearnOpenGL Split-Sum IBL 使用的三类预计算资源。
// 这是启动期离线步骤，运行时 Shader 只做纹理查询，不再每像素重复积分。
[[nodiscard]] IblTextures BuildSplitSumIbl(
    const HdrCubemap& source,
    std::uint32_t irradianceSize = 32,
    std::uint32_t prefilterSize = 128,
    std::uint32_t prefilterMipCount = 5,
    std::uint32_t brdfSize = 256);
} // namespace Shadow::Assets
