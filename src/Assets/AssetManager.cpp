#include "Assets/AssetManager.h"

#include "Assets/GltfLoader.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <system_error>

namespace Shadow::Assets
{
AssetManager::AssetManager()
{
    CreateBuiltInMeshes();
}

bool AssetManager::ImportGltf(
    const std::filesystem::path& path,
    std::vector<std::string>& importedKeys,
    std::string& errorMessage)
{
    std::vector<MeshAsset> importedMeshes;
    if (!GltfLoader::Load(path, importedMeshes, errorMessage))
    {
        return false;
    }

    importedKeys.clear();
    for (MeshAsset& mesh : importedMeshes)
    {
        importedKeys.push_back(mesh.key);
        meshes_.insert_or_assign(mesh.key, std::move(mesh));
    }
    ++revision_;
    return true;
}

const MeshAsset* AssetManager::FindMesh(const std::string& key) const noexcept
{
    const auto iterator = meshes_.find(key);
    return iterator == meshes_.end() ? nullptr : &iterator->second;
}

std::vector<std::string> AssetManager::MeshKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(meshes_.size());
    for (const auto& [key, mesh] : meshes_)
    {
        static_cast<void>(mesh);
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    return keys;
}

std::vector<ImportedModelAsset> AssetManager::ImportedModels() const
{
    std::unordered_map<std::string, ImportedModelAsset> modelsByUri;
    for (const auto& [key, mesh] : meshes_)
    {
        static_cast<void>(mesh);
        if (!key.starts_with("gltf://"))
        {
            continue;
        }

        const std::size_t fragment = key.find('#');
        const std::string uri = key.substr(0, fragment);
        const std::string sourcePath = uri.substr(7);
        ImportedModelAsset& model = modelsByUri[uri];
        if (model.uri.empty())
        {
            model.uri = uri;
            model.sourcePath = sourcePath;
            model.displayName = std::filesystem::path(sourcePath).stem().string();
        }
        model.meshKeys.push_back(key);
    }

    std::vector<ImportedModelAsset> models;
    models.reserve(modelsByUri.size());
    for (auto& [uri, model] : modelsByUri)
    {
        static_cast<void>(uri);
        std::ranges::sort(model.meshKeys);
        models.push_back(std::move(model));
    }
    std::ranges::sort(models, {}, &ImportedModelAsset::sourcePath);
    return models;
}

void AssetManager::DiscoverGltfAssets(
    const std::filesystem::path& modelsDirectory,
    std::string& warningMessage)
{
    warningMessage.clear();
    if (!std::filesystem::exists(modelsDirectory))
    {
        std::filesystem::create_directories(modelsDirectory);
        return;
    }

    std::vector<std::filesystem::path> assetPaths;
    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator iterator(
        modelsDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        iterationError);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iterationError))
    {
        if (iterationError)
        {
            warningMessage += "Could not scan asset directory: " +
                iterationError.message() + "\n";
            iterationError.clear();
            continue;
        }
        const std::filesystem::directory_entry& entry = *iterator;
        if (!entry.is_regular_file())
        {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        if (extension == ".glb" || extension == ".gltf")
        {
            assetPaths.push_back(entry.path());
        }
    }
    std::ranges::sort(assetPaths);

    for (const std::filesystem::path& path : assetPaths)
    {
        // 场景只保存相对仓库的可移植路径，不能把当前电脑的绝对路径写进 assetKey。
        std::error_code relativeError;
        std::filesystem::path importPath =
            std::filesystem::relative(path, std::filesystem::current_path(), relativeError);
        if (relativeError)
        {
            importPath = path.lexically_normal();
        }
        std::vector<std::string> importedKeys;
        std::string error;
        if (!ImportGltf(importPath, importedKeys, error))
        {
            warningMessage += "Could not discover " + path.generic_string() +
                ": " + error + "\n";
        }
    }
}

void AssetManager::LoadReferencedAssets(const Scene::Scene& scene, std::string& warningMessage)
{
    warningMessage.clear();
    for (const Scene::SceneObject& object : scene.Objects())
    {
        if (FindMesh(object.assetKey) != nullptr || !object.assetKey.starts_with("gltf://"))
        {
            continue;
        }
        const std::size_t fragment = object.assetKey.find('#');
        const std::string path = object.assetKey.substr(7, fragment - 7);
        std::vector<std::string> importedKeys;
        std::string error;
        if (!ImportGltf(path, importedKeys, error))
        {
            warningMessage += "Could not reload " + path + ": " + error + "\n";
        }
    }
}

void AssetManager::CreateBuiltInMeshes()
{
    MeshAsset sphere;
    sphere.key = "builtin://sphere";
    constexpr std::uint32_t latitudeSegments = 32;
    constexpr std::uint32_t longitudeSegments = 48;
    constexpr float pi = std::numbers::pi_v<float>;
    for (std::uint32_t latitude = 0; latitude <= latitudeSegments; ++latitude)
    {
        const float v = static_cast<float>(latitude) / latitudeSegments;
        const float phi = v * pi;
        const float y = std::cos(phi);
        const float ringRadius = std::sin(phi);
        for (std::uint32_t longitude = 0; longitude <= longitudeSegments; ++longitude)
        {
            const float u = static_cast<float>(longitude) / longitudeSegments;
            const float theta = u * 2.0F * pi;
            const float x = ringRadius * std::sin(theta);
            const float z = ringRadius * std::cos(theta);
            const DirectX::XMFLOAT3 tangent{
                std::cos(theta), 0.0F, -std::sin(theta)};
            sphere.vertices.push_back({
                {x, y, z}, {x, y, z}, {u, v}, {tangent.x, tangent.y, tangent.z, 1.0F}});
        }
    }
    const std::uint32_t ringVertexCount = longitudeSegments + 1;
    for (std::uint32_t latitude = 0; latitude < latitudeSegments; ++latitude)
    {
        for (std::uint32_t longitude = 0; longitude < longitudeSegments; ++longitude)
        {
            const std::uint32_t current = latitude * ringVertexCount + longitude;
            const std::uint32_t next = current + ringVertexCount;
            sphere.indices.insert(sphere.indices.end(), {
                current, next, current + 1,
                current + 1, next, next + 1,
            });
        }
    }
    meshes_.emplace(sphere.key, std::move(sphere));

    MeshAsset ground;
    ground.key = "builtin://ground";
    ground.boundsExtents = {4.0F, 0.01F, 4.0F};
    ground.vertices = {
        {{-4.0F, 0.0F, -4.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{-4.0F, 0.0F,  4.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 4.0F, 0.0F,  4.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 4.0F, 0.0F, -4.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    ground.indices = {0, 1, 2, 0, 2, 3};
    meshes_.emplace(ground.key, std::move(ground));

    // 小型 Plane 适合做测试台，Ground 则保留原来的 8x8 场景地面。
    MeshAsset plane;
    plane.key = "builtin://plane";
    plane.boundsExtents = {1.0F, 0.01F, 1.0F};
    plane.vertices = {
        {{-1.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{-1.0F, 0.0F,  1.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 1.0F, 0.0F,  1.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{ 1.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    plane.indices = {0, 1, 2, 0, 2, 3};
    meshes_.emplace(plane.key, std::move(plane));

    // Cube 使用每个面独立的顶点，保证法线、切线和 UV 在棱边处不互相污染。
    MeshAsset cube;
    cube.key = "builtin://cube";
    cube.boundsExtents = {1.0F, 1.0F, 1.0F};
    const auto addCubeFace = [&](const DirectX::XMFLOAT3& normal,
                                 const DirectX::XMFLOAT3& tangent,
                                 const std::array<DirectX::XMFLOAT3, 4>& positions)
    {
        const std::uint32_t first = static_cast<std::uint32_t>(cube.vertices.size());
        const std::array<DirectX::XMFLOAT2, 4> uvs = {
            DirectX::XMFLOAT2{0.0F, 1.0F}, DirectX::XMFLOAT2{0.0F, 0.0F},
            DirectX::XMFLOAT2{1.0F, 0.0F}, DirectX::XMFLOAT2{1.0F, 1.0F}};
        for (std::size_t index = 0; index < positions.size(); ++index)
        {
            cube.vertices.push_back({
                positions[index], normal, uvs[index],
                {tangent.x, tangent.y, tangent.z, 1.0F}});
        }
        cube.indices.insert(cube.indices.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3});
    };
    addCubeFace(
        {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F},
        {{{-1.0F, -1.0F, 1.0F}, {-1.0F, 1.0F, 1.0F},
          {1.0F, 1.0F, 1.0F}, {1.0F, -1.0F, 1.0F}}});
    addCubeFace(
        {0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F},
        {{{1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, -1.0F},
          {-1.0F, 1.0F, -1.0F}, {-1.0F, -1.0F, -1.0F}}});
    addCubeFace(
        {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F},
        {{{1.0F, -1.0F, 1.0F}, {1.0F, 1.0F, 1.0F},
          {1.0F, 1.0F, -1.0F}, {1.0F, -1.0F, -1.0F}}});
    addCubeFace(
        {-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F},
        {{{-1.0F, -1.0F, -1.0F}, {-1.0F, 1.0F, -1.0F},
          {-1.0F, 1.0F, 1.0F}, {-1.0F, -1.0F, 1.0F}}});
    addCubeFace(
        {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
        {{{-1.0F, 1.0F, 1.0F}, {-1.0F, 1.0F, -1.0F},
          {1.0F, 1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}}});
    addCubeFace(
        {0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
        {{{-1.0F, -1.0F, -1.0F}, {-1.0F, -1.0F, 1.0F},
          {1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, -1.0F}}});
    meshes_.emplace(cube.key, std::move(cube));
}
} // namespace Shadow::Assets
