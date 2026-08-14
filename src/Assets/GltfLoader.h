#pragma once

#include "Assets/MeshAsset.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Shadow::Assets
{
class GltfLoader final
{
public:
    [[nodiscard]] static bool Load(
        const std::filesystem::path& path,
        std::vector<MeshAsset>& meshes,
        std::string& errorMessage) noexcept;
};
} // namespace Shadow::Assets
