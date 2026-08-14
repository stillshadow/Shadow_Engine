#include "Assets/AssetManager.h"
#include "Editor/ScenePicking.h"
#include "Scene/Scene.h"

#include <DirectXMath.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main()
{
    using namespace DirectX;
    constexpr float viewportWidth = 1280.0F;
    constexpr float viewportHeight = 720.0F;
    const XMVECTOR cameraPosition = XMVectorSet(3.0F, 2.2F, -5.0F, 1.0F);
    const XMMATRIX view = XMMatrixLookAtLH(
        cameraPosition,
        XMVectorSet(0.0F, 0.72F, 0.0F, 1.0F),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(55.0F), viewportWidth / viewportHeight, 0.1F, 100.0F);

    Shadow::Scene::Scene scene;
    Shadow::Assets::AssetManager assets;

    // 自动发现遇到单个损坏 GLB 时应记录 Warning，但不能丢失内置资产或中断启动流程。
    const std::filesystem::path discoveryDirectory =
        std::filesystem::temp_directory_path() / "ShadowEngineAssetDiscoveryTest";
    std::filesystem::create_directories(discoveryDirectory / "Nested");
    const std::filesystem::path invalidAsset =
        discoveryDirectory / "Nested" / "Invalid.glb";
    {
        std::ofstream output(invalidAsset, std::ios::binary);
        output << "not a glb";
    }

    std::string discoveryWarnings;
    assets.DiscoverGltfAssets(discoveryDirectory, discoveryWarnings);
    if (discoveryWarnings.empty() || assets.FindMesh("builtin://sphere") == nullptr)
    {
        std::cerr << "Asset discovery did not isolate an invalid GLB.\n";
        return 1;
    }
    std::filesystem::remove_all(discoveryDirectory);

    const auto& sphere = scene.Objects()[0];
    const XMVECTOR projectedCenter = XMVector3Project(
        XMLoadFloat3(&sphere.transform.position),
        0.0F, 0.0F, viewportWidth, viewportHeight, 0.0F, 1.0F,
        projection, view, XMMatrixIdentity());

    const auto picked = Shadow::Editor::PickSceneObject(
        scene,
        assets,
        XMVectorGetX(projectedCenter),
        XMVectorGetY(projectedCenter),
        viewportWidth,
        viewportHeight,
        view,
        projection);
    if (!picked.has_value() || *picked != 0)
    {
        std::cerr << "Projected center did not pick the nearest sphere.\n";
        return 1;
    }

    // 光源没有 Mesh；编辑器使用屏幕空间图标范围拾取，并跳过没有位置的 Directional Light。
    scene.Lights().push_back({
        "Picking Point",
        Shadow::Scene::LightType::Point,
        {0.0F, -1.0F, 0.0F},
        {0.0F, 1.5F, 0.0F},
        {1.0F, 1.0F, 1.0F},
        2.0F,
        5.0F,
        1.0F,
        1.0F,
    });
    const XMVECTOR projectedLight = XMVector3Project(
        XMLoadFloat3(&scene.Lights().back().position),
        0.0F, 0.0F, viewportWidth, viewportHeight, 0.0F, 1.0F,
        projection, view, XMMatrixIdentity());
    const auto pickedLight = Shadow::Editor::PickSceneLight(
        scene,
        XMVectorGetX(projectedLight),
        XMVectorGetY(projectedLight),
        viewportWidth,
        viewportHeight,
        view,
        projection);
    if (!pickedLight.has_value() || *pickedLight != scene.Lights().size() - 1)
    {
        std::cerr << "Projected light icon was not picked.\n";
        return 1;
    }

    return 0;
}
