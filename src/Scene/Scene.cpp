#include "Scene/Scene.h"

namespace Shadow::Scene
{
DirectX::XMMATRIX TransformMatrix(const Transform& transform) noexcept
{
    using namespace DirectX;
    return XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z) *
           XMMatrixRotationRollPitchYaw(
               XMConvertToRadians(transform.rotationDegrees.x),
               XMConvertToRadians(transform.rotationDegrees.y),
               XMConvertToRadians(transform.rotationDegrees.z)) *
           XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
}

DirectX::XMMATRIX NormalMatrix(const DirectX::FXMMATRIX model) noexcept
{
    // Position/Tangent 按普通 Model Matrix 变换。Normal 使用逆转置，才能补偿非均匀缩放：
    // 某一轴把表面切线拉长时，法线需要按该轴的倒数变化，继续保持与表面垂直。
    return DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, model));
}

Scene::Scene()
{
    ResetToDefaults();
}

void Scene::ResetToDefaults()
{
    // Scene 只保存可编辑的数据，不再混入 Vertex Buffer 范围或 DX12 Handle。
    // Renderer 会根据 MeshType 决定绘制哪一段网格。
    objects_ = {
        {
            "Sphere 1 - Blue",
            "builtin://sphere",
            {{0.0F, 0.75F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},
            {{0.12F, 0.34F, 0.82F, 1.0F}, 0.22F, 0.0F, 1.0F},
        },
        {
            "Sphere 2 - Metal",
            "builtin://sphere",
            {{1.35F, 0.54F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.72F, 0.72F, 0.72F}},
            {{0.82F, 0.18F, 0.06F, 1.0F}, 0.32F, 1.0F, 1.0F},
        },
        {
            "Ground",
            "builtin://ground",
            {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},
            {{0.18F, 0.20F, 0.23F, 1.0F}, 0.85F, 0.0F, 1.0F},
        },
    };
    environment_ = {};
    // 第一盏 Directional Light 也就是默认的阴影投射光源，方向固定且可在编辑器中修改。
    lights_ = {
        {
            "Key Directional",
            LightType::Directional,
            {0.35F, 0.80F, 0.45F},
            {0.0F, 5.0F, 4.0F},
            {1.0F, 0.92F, 0.80F},
            4.0F,
            8.0F,
            2.0F,
            2.0F,
        },
    };
}
} // namespace Shadow::Scene
