#pragma once

#include <DirectXMath.h>

namespace Shadow::Editor
{
class EditorCamera final
{
public:
    void UpdateInput(bool allowMouseInput);
    void Focus(const DirectX::XMFLOAT3& target, float objectRadius) noexcept;

    [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept;
    [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix(float aspectRatio) const noexcept;
    [[nodiscard]] DirectX::XMFLOAT3 Position() const noexcept;

private:
    DirectX::XMFLOAT3 target_{0.0F, 0.72F, 0.0F};
    float yawRadians_ = 0.5404F;
    float pitchRadians_ = 0.2480F;
    float distance_ = 6.02F;
};
} // namespace Shadow::Editor
