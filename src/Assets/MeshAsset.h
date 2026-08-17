#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Shadow::Assets
{
struct MeshVertex
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{0.0F, 1.0F, 0.0F};
    DirectX::XMFLOAT2 uv{};
    DirectX::XMFLOAT4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
};

struct CpuTexture
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return width > 0 && height > 0 && rgba8.size() ==
            static_cast<std::size_t>(width) * height * 4;
    }
};

struct ImportedMaterial
{
    DirectX::XMFLOAT4 baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    DirectX::XMFLOAT3 emissiveFactor{0.0F, 0.0F, 0.0F};
    float roughnessFactor = 1.0F;
    float metallicFactor = 0.0F;
    CpuTexture baseColorTexture;
    CpuTexture normalTexture;
    CpuTexture metallicRoughnessTexture;
    CpuTexture emissiveTexture;
};

struct MeshAsset
{
    std::string key;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    ImportedMaterial importedMaterial;
    DirectX::XMFLOAT3 boundsCenter{};
    DirectX::XMFLOAT3 boundsExtents{1.0F, 1.0F, 1.0F};
};
} // namespace Shadow::Assets
