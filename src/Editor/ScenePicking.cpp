#include "Editor/ScenePicking.h"

#include "Assets/AssetManager.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace DirectX;

bool IntersectBounds(
    const XMVECTOR localOrigin,
    const XMVECTOR localDirection,
    const XMFLOAT3& center,
    const XMFLOAT3& extents,
    XMVECTOR& localHitPoint) noexcept
{
    XMFLOAT3 origin{};
    XMFLOAT3 direction{};
    XMStoreFloat3(&origin, localOrigin);
    XMStoreFloat3(&direction, localDirection);
    const float origins[] = {origin.x, origin.y, origin.z};
    const float directions[] = {direction.x, direction.y, direction.z};
    const float minimums[] = {
        center.x - extents.x, center.y - extents.y, center.z - extents.z};
    const float maximums[] = {
        center.x + extents.x, center.y + extents.y, center.z + extents.z};
    float nearDistance = 0.0F;
    float farDistance = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(directions[axis]) < 0.00001F)
        {
            if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
            {
                return false;
            }
            continue;
        }
        const float inverseDirection = 1.0F / directions[axis];
        float first = (minimums[axis] - origins[axis]) * inverseDirection;
        float second = (maximums[axis] - origins[axis]) * inverseDirection;
        if (first > second)
        {
            std::swap(first, second);
        }
        nearDistance = std::max(nearDistance, first);
        farDistance = std::min(farDistance, second);
        if (nearDistance > farDistance)
        {
            return false;
        }
    }
    localHitPoint = XMVectorMultiplyAdd(
        XMVectorReplicate(nearDistance), localDirection, localOrigin);
    return true;
}

bool ProjectPoint(
    const XMFLOAT3& point,
    const float viewportWidth,
    const float viewportHeight,
    const XMMATRIX& view,
    const XMMATRIX& projection,
    XMFLOAT3& screenPoint) noexcept
{
    XMStoreFloat3(
        &screenPoint,
        XMVector3Project(
            XMLoadFloat3(&point),
            0.0F, 0.0F, viewportWidth, viewportHeight, 0.0F, 1.0F,
            projection, view, XMMatrixIdentity()));
    return std::isfinite(screenPoint.x) && std::isfinite(screenPoint.y) &&
        std::isfinite(screenPoint.z) && screenPoint.z >= 0.0F && screenPoint.z <= 1.0F;
}

void BuildAreaBasis(
    const XMFLOAT3& direction,
    XMVECTOR& right,
    XMVECTOR& up) noexcept
{
    XMVECTOR normal = XMLoadFloat3(&direction);
    const float lengthSquared = XMVectorGetX(XMVector3LengthSq(normal));
    normal = lengthSquared > 0.000001F
        ? XMVector3Normalize(normal)
        : XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F);
    const XMVECTOR helper = std::abs(XMVectorGetY(normal)) < 0.95F
        ? XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F)
        : XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
    right = XMVector3Normalize(XMVector3Cross(helper, normal));
    up = XMVector3Normalize(XMVector3Cross(normal, right));
}
} // namespace

namespace Shadow::Editor
{
std::optional<std::size_t> PickSceneObject(
    const Scene::Scene& scene,
    const Assets::AssetManager& assets,
    const float mouseX,
    const float mouseY,
    const float viewportWidth,
    const float viewportHeight,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection) noexcept
{
    using namespace DirectX;
    if (viewportWidth <= 0.0F || viewportHeight <= 0.0F ||
        mouseX < 0.0F || mouseY < 0.0F ||
        mouseX >= viewportWidth || mouseY >= viewportHeight)
    {
        return std::nullopt;
    }

    const XMMATRIX identity = XMMatrixIdentity();
    const XMVECTOR nearPoint = XMVector3Unproject(
        XMVectorSet(mouseX, mouseY, 0.0F, 1.0F),
        0.0F, 0.0F, viewportWidth, viewportHeight, 0.0F, 1.0F,
        projection, view, identity);
    const XMVECTOR farPoint = XMVector3Unproject(
        XMVectorSet(mouseX, mouseY, 1.0F, 1.0F),
        0.0F, 0.0F, viewportWidth, viewportHeight, 0.0F, 1.0F,
        projection, view, identity);
    const XMVECTOR worldDirection = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

    std::optional<std::size_t> nearestObject;
    float nearestDistance = std::numeric_limits<float>::max();
    const auto& objects = scene.Objects();
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const Scene::SceneObject& object = objects[index];
        const Assets::MeshAsset* mesh = assets.FindMesh(object.assetKey);
        if (mesh == nullptr)
        {
            continue;
        }
        const XMMATRIX model = Scene::TransformMatrix(object.transform);
        const XMMATRIX inverseModel = XMMatrixInverse(nullptr, model);
        const XMVECTOR localOrigin = XMVector3TransformCoord(nearPoint, inverseModel);
        const XMVECTOR localDirection = XMVector3Normalize(
            XMVector3TransformNormal(worldDirection, inverseModel));

        XMVECTOR localHitPoint{};
        const bool hit = IntersectBounds(
            localOrigin, localDirection,
            mesh->boundsCenter, mesh->boundsExtents, localHitPoint);
        if (!hit)
        {
            continue;
        }

        const XMVECTOR worldHitPoint = XMVector3TransformCoord(localHitPoint, model);
        const float worldDistance = XMVectorGetX(
            XMVector3Length(XMVectorSubtract(worldHitPoint, nearPoint)));
        if (worldDistance < nearestDistance)
        {
            nearestDistance = worldDistance;
            nearestObject = index;
        }
    }

    return nearestObject;
}

std::optional<std::size_t> PickSceneLight(
    const Scene::Scene& scene,
    const float mouseX,
    const float mouseY,
    const float viewportWidth,
    const float viewportHeight,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection) noexcept
{
    using namespace DirectX;
    if (viewportWidth <= 0.0F || viewportHeight <= 0.0F ||
        mouseX < 0.0F || mouseY < 0.0F ||
        mouseX >= viewportWidth || mouseY >= viewportHeight)
    {
        return std::nullopt;
    }

    std::optional<std::size_t> nearestLight;
    float nearestDepth = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < scene.Lights().size(); ++index)
    {
        const Scene::SceneLight& light = scene.Lights()[index];
        if (light.type == Scene::LightType::Directional)
        {
            continue;
        }

        XMFLOAT3 centerScreen{};
        if (!ProjectPoint(
                light.position, viewportWidth, viewportHeight,
                view, projection, centerScreen))
        {
            continue;
        }

        // 点光源和聚光灯中心图标使用固定像素命中范围；这样远近变化不会让图标难以点中。
        constexpr float iconPickHalfSize = 22.0F;
        float minimumX = centerScreen.x - iconPickHalfSize;
        float maximumX = centerScreen.x + iconPickHalfSize;
        float minimumY = centerScreen.y - iconPickHalfSize;
        float maximumY = centerScreen.y + iconPickHalfSize;
        if (light.type == Scene::LightType::Area)
        {
            // Area Light 的整块矩形都可以点击，而不只是中心点。
            XMVECTOR right{};
            XMVECTOR up{};
            BuildAreaBasis(light.direction, right, up);
            const XMVECTOR center = XMLoadFloat3(&light.position);
            const float halfWidth = std::max(light.width * 0.5F, 0.01F);
            const float halfHeight = std::max(light.height * 0.5F, 0.01F);
            const std::array<XMVECTOR, 4> corners = {
                XMVectorAdd(center, XMVectorAdd(
                    XMVectorScale(right, -halfWidth), XMVectorScale(up, -halfHeight))),
                XMVectorAdd(center, XMVectorAdd(
                    XMVectorScale(right, halfWidth), XMVectorScale(up, -halfHeight))),
                XMVectorAdd(center, XMVectorAdd(
                    XMVectorScale(right, halfWidth), XMVectorScale(up, halfHeight))),
                XMVectorAdd(center, XMVectorAdd(
                    XMVectorScale(right, -halfWidth), XMVectorScale(up, halfHeight))),
            };
            for (const XMVECTOR corner : corners)
            {
                XMFLOAT3 worldCorner{};
                XMFLOAT3 screenCorner{};
                XMStoreFloat3(&worldCorner, corner);
                if (ProjectPoint(
                        worldCorner, viewportWidth, viewportHeight,
                        view, projection, screenCorner))
                {
                    minimumX = std::min(minimumX, screenCorner.x - 8.0F);
                    maximumX = std::max(maximumX, screenCorner.x + 8.0F);
                    minimumY = std::min(minimumY, screenCorner.y - 8.0F);
                    maximumY = std::max(maximumY, screenCorner.y + 8.0F);
                }
            }
        }
        else if (light.type == Scene::LightType::Spot)
        {
            // 聚光灯的锥体边缘也是可见的编辑器图形，允许点击锥体任意一条连线选中它。
            XMVECTOR right{};
            XMVECTOR up{};
            BuildAreaBasis(light.direction, right, up);
            const XMVECTOR center = XMLoadFloat3(&light.position);
            XMVECTOR direction = XMLoadFloat3(&light.direction);
            direction = XMVectorGetX(XMVector3LengthSq(direction)) > 0.000001F
                ? XMVector3Normalize(direction)
                : XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F);
            const float coneLength = std::min(std::max(light.range, 0.1F), 4.0F);
            const float coneRadius = std::tan(XMConvertToRadians(light.outerConeDegrees)) *
                coneLength;
            const XMVECTOR coneCenter = XMVectorAdd(
                center, XMVectorScale(direction, coneLength));
            const std::array<XMVECTOR, 4> rimPoints = {
                XMVectorAdd(coneCenter, XMVectorScale(right, coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(right, -coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(up, coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(up, -coneRadius)),
            };
            for (const XMVECTOR rimPoint : rimPoints)
            {
                XMFLOAT3 worldPoint{};
                XMFLOAT3 screenPoint{};
                XMStoreFloat3(&worldPoint, rimPoint);
                if (ProjectPoint(
                        worldPoint, viewportWidth, viewportHeight,
                        view, projection, screenPoint))
                {
                    minimumX = std::min(minimumX, screenPoint.x - 8.0F);
                    maximumX = std::max(maximumX, screenPoint.x + 8.0F);
                    minimumY = std::min(minimumY, screenPoint.y - 8.0F);
                    maximumY = std::max(maximumY, screenPoint.y + 8.0F);
                }
            }
        }

        const bool hit = mouseX >= minimumX && mouseX <= maximumX &&
            mouseY >= minimumY && mouseY <= maximumY;
        if (hit && centerScreen.z < nearestDepth)
        {
            nearestDepth = centerScreen.z;
            nearestLight = index;
        }
    }
    return nearestLight;
}
} // namespace Shadow::Editor
