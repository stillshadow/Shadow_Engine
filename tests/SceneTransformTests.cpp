#include "Scene/Scene.h"

#include <DirectXMath.h>

#include <cmath>
#include <iostream>

namespace
{
bool NearlyPerpendicular(
    const DirectX::FXMVECTOR left,
    const DirectX::FXMVECTOR right) noexcept
{
    const float dot = DirectX::XMVectorGetX(DirectX::XMVector3Dot(
        DirectX::XMVector3Normalize(left), DirectX::XMVector3Normalize(right)));
    return std::abs(dot) < 0.0001F;
}
}

int main()
{
    using namespace DirectX;

    const Shadow::Scene::Transform transform{
        {4.0F, -2.0F, 1.0F},
        {23.0F, -41.0F, 17.0F},
        {2.0F, 0.5F, 3.0F},
    };
    const XMMATRIX model = Shadow::Scene::TransformMatrix(transform);
    const XMMATRIX normalMatrix = Shadow::Scene::NormalMatrix(model);

    // 选择一组不与坐标轴重合的局部切线，并由叉乘得到与二者垂直的局部法线。
    const XMVECTOR localTangent = XMVector3Normalize(
        XMVectorSet(1.0F, 2.0F, -0.5F, 0.0F));
    const XMVECTOR localBitangent = XMVector3Normalize(
        XMVectorSet(-0.75F, 0.25F, 1.5F, 0.0F));
    const XMVECTOR localNormal = XMVector3Normalize(
        XMVector3Cross(localTangent, localBitangent));

    const XMVECTOR worldTangent = XMVector3TransformNormal(localTangent, model);
    const XMVECTOR worldBitangent = XMVector3TransformNormal(localBitangent, model);
    const XMVECTOR worldNormal = XMVector3TransformNormal(localNormal, normalMatrix);

    if (!NearlyPerpendicular(worldNormal, worldTangent) ||
        !NearlyPerpendicular(worldNormal, worldBitangent))
    {
        std::cerr << "Inverse-transpose normal is not perpendicular after non-uniform scale.\n";
        return 1;
    }

    return 0;
}
