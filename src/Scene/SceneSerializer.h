#pragma once

#include <filesystem>
#include <string>

namespace Shadow::Scene
{
class Scene;

class SceneSerializer final
{
public:
    [[nodiscard]] static std::filesystem::path DefaultScenePath();
    [[nodiscard]] static bool Load(
        const std::filesystem::path& path, Scene& scene, std::string& errorMessage) noexcept;
    [[nodiscard]] static bool Save(
        const std::filesystem::path& path, const Scene& scene, std::string& errorMessage) noexcept;
};
} // namespace Shadow::Scene
