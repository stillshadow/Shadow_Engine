#pragma once

#include "Assets/MeshAsset.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Shadow::Scene
{
class Scene;
}

namespace Shadow::Assets
{
struct ImportedModelAsset
{
    // 一个 GLB 文件可能拆成多个 Mesh Primitive；编辑器只把它显示成一个模型条目。
    std::string uri;
    std::string displayName;
    std::string sourcePath;
    std::vector<std::string> meshKeys;
};

class AssetManager final
{
public:
    AssetManager();

    [[nodiscard]] bool ImportGltf(
        const std::filesystem::path& path,
        std::vector<std::string>& importedKeys,
        std::string& errorMessage);
    [[nodiscard]] const MeshAsset* FindMesh(const std::string& key) const noexcept;
    [[nodiscard]] std::vector<std::string> MeshKeys() const;
    [[nodiscard]] std::vector<ImportedModelAsset> ImportedModels() const;
    // 启动时递归扫描标准运行时目录。单个损坏资产只记录 Warning，不阻止其他资产加载。
    void DiscoverGltfAssets(
        const std::filesystem::path& modelsDirectory,
        std::string& warningMessage);
    void LoadReferencedAssets(const Scene::Scene& scene, std::string& warningMessage);
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

private:
    void CreateBuiltInMeshes();

    std::unordered_map<std::string, MeshAsset> meshes_;
    std::uint64_t revision_ = 1;
};
} // namespace Shadow::Assets
