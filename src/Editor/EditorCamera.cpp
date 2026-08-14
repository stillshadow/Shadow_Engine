#include "Editor/EditorCamera.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace Shadow::Editor
{
void EditorCamera::UpdateInput(const bool allowMouseInput)
{
    if (!allowMouseInput)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    constexpr float orbitSpeed = 0.006F;
    // Blender 风格导航：MMB 环绕，Shift+MMB 平移。两个操作共享同一个按键，
    // 因此先判断 Shift，避免一次拖动同时修改旋转角和观察中心。
    const bool middleMouseDragging = ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    if (middleMouseDragging && !io.KeyShift)
    {
        // Blender 的手感更像“拖动画面”：鼠标向右时场景也向右转，因此相机角度取反。
        // Blender 风格是“拖动画面”：水平拖动需要反转 Yaw，垂直拖动则与 Pitch 同向。
        yawRadians_ -= io.MouseDelta.x * orbitSpeed;
        pitchRadians_ += io.MouseDelta.y * orbitSpeed;
        pitchRadians_ = std::clamp(pitchRadians_, -1.45F, 1.45F);
    }

    if (middleMouseDragging && io.KeyShift)
    {
        using namespace DirectX;
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, ViewMatrix());
        const XMVECTOR cameraRight = XMVector3Normalize(inverseView.r[0]);
        const XMVECTOR cameraUp = XMVector3Normalize(inverseView.r[1]);
        const float panScale = distance_ * 0.0015F;
        XMVECTOR target = XMLoadFloat3(&target_);
        target = XMVectorAdd(target, XMVectorScale(cameraRight, -io.MouseDelta.x * panScale));
        target = XMVectorAdd(target, XMVectorScale(cameraUp, io.MouseDelta.y * panScale));
        XMStoreFloat3(&target_, target);
    }

    if (io.MouseWheel != 0.0F)
    {
        // 指数缩放让近距离移动细腻、远距离移动更快，手感比固定步长稳定。
        distance_ *= std::exp(-io.MouseWheel * 0.15F);
        distance_ = std::clamp(distance_, 0.25F, 100.0F);
    }
}

void EditorCamera::Focus(const DirectX::XMFLOAT3& target, const float objectRadius) noexcept
{
    target_ = target;
    distance_ = std::clamp(objectRadius * 3.5F, 1.5F, 30.0F);
}

DirectX::XMFLOAT3 EditorCamera::Position() const noexcept
{
    const float horizontalDistance = distance_ * std::cos(pitchRadians_);
    return {
        target_.x + horizontalDistance * std::sin(yawRadians_),
        target_.y + distance_ * std::sin(pitchRadians_),
        target_.z - horizontalDistance * std::cos(yawRadians_),
    };
}

DirectX::XMMATRIX EditorCamera::ViewMatrix() const noexcept
{
    using namespace DirectX;
    const XMFLOAT3 position = Position();
    return XMMatrixLookAtLH(
        XMLoadFloat3(&position),
        XMLoadFloat3(&target_),
        XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
}

DirectX::XMMATRIX EditorCamera::ProjectionMatrix(const float aspectRatio) const noexcept
{
    return DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(55.0F), aspectRatio, 0.1F, 100.0F);
}
} // namespace Shadow::Editor
