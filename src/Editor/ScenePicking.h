#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <optional>

namespace Shadow::Scene
{
class Scene;
}
namespace Shadow::Assets
{
class AssetManager;
}

namespace Shadow::Editor
{
[[nodiscard]] std::optional<std::size_t> PickSceneObject(
    const Scene::Scene& scene,
    const Assets::AssetManager& assets,
    float mouseX,
    float mouseY,
    float viewportWidth,
    float viewportHeight,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection) noexcept;

// 光源图标属于编辑器叠加层，没有真实 Mesh；因此使用它在屏幕上的可视范围拾取。
// 返回 Scene::Lights() 中的索引，Directional Light 因为没有位置而不会参与。
[[nodiscard]] std::optional<std::size_t> PickSceneLight(
    const Scene::Scene& scene,
    float mouseX,
    float mouseY,
    float viewportWidth,
    float viewportHeight,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection) noexcept;
} // namespace Shadow::Editor
