#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Shadow::Scene
{
struct Transform
{
    DirectX::XMFLOAT3 position{0.0F, 0.0F, 0.0F};
    DirectX::XMFLOAT3 rotationDegrees{0.0F, 0.0F, 0.0F};
    DirectX::XMFLOAT3 scale{1.0F, 1.0F, 1.0F};
};

[[nodiscard]] DirectX::XMMATRIX TransformMatrix(const Transform& transform) noexcept;
// 法线必须使用 Model 的逆转置矩阵，才能在 X/Y/Z 缩放不相等时仍与表面切线垂直。
[[nodiscard]] DirectX::XMMATRIX NormalMatrix(DirectX::FXMMATRIX model) noexcept;

struct PbrMaterial
{
    DirectX::XMFLOAT4 baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    float roughness = 0.5F;
    float metallic = 0.0F;
    float normalStrength = 1.0F;
    // 当前资产契约没有单独的 Height Map；大于 0 时先复用法线纹理的红色通道做视差高度。
    float parallaxHeightScale = 0.0F;
};

struct SceneObject
{
    std::string name;
    // 一个 SceneObject 对应一个 Mesh 和一套材质，保持作品阶段的数据流简单直观。
    std::string assetKey = "builtin://sphere";
    Transform transform;
    PbrMaterial material;
};

enum class LightType : std::uint32_t
{
    Directional = 0,
    Point = 1,
    Area = 2,
    Spot = 3,
};

enum class RenderPath : std::uint32_t
{
    Forward = 0,
    Deferred = 1,
};

struct SceneLight
{
    std::string name;
    LightType type = LightType::Directional;
    // Directional 光使用 direction；Area 光把它当作发光面朝向场景的方向。
    DirectX::XMFLOAT3 direction{0.35F, 0.80F, 0.45F};
    DirectX::XMFLOAT3 position{0.0F, 3.0F, 2.0F};
    DirectX::XMFLOAT3 color{1.0F, 0.92F, 0.80F};
    float intensity = 4.0F;
    float range = 8.0F;
    float width = 2.0F;
    float height = 2.0F;
    float innerConeDegrees = 20.0F;
    float outerConeDegrees = 30.0F;
};

struct EnvironmentSettings
{
    // HDRI 天空与 IBL 的整体亮度和水平旋转角。它们属于场景外观，因此与物体一起保存。
    float intensity = 1.0F;
    float rotationDegrees = 0.0F;
    float exposure = 1.0F;
    float iblSpecularStrength = 0.25F;
    float ssaoStrength = 0.0F;
    bool bloomEnabled = true;
    float bloomThreshold = 1.0F;
    float bloomStrength = 0.08F;
    // MSAA 只作用于 Forward 几何路径；Deferred 使用单采样 GBuffer，并由后处理负责抗锯齿扩展。
    bool msaaEnabled = false;
    bool showGeometryNormals = false;
    // 两条路径共用同一套场景、材质和光照参数，便于一键对比管线组织方式。
    RenderPath renderPath = RenderPath::Forward;
};

class Scene final
{
public:
    Scene();

    [[nodiscard]] std::vector<SceneObject>& Objects() noexcept { return objects_; }
    [[nodiscard]] const std::vector<SceneObject>& Objects() const noexcept
    {
        return objects_;
    }
    [[nodiscard]] EnvironmentSettings& Environment() noexcept { return environment_; }
    [[nodiscard]] const EnvironmentSettings& Environment() const noexcept
    {
        return environment_;
    }
    [[nodiscard]] std::vector<SceneLight>& Lights() noexcept { return lights_; }
    [[nodiscard]] const std::vector<SceneLight>& Lights() const noexcept
    {
        return lights_;
    }
    void ResetToDefaults();

private:
    std::vector<SceneObject> objects_;
    EnvironmentSettings environment_;
    std::vector<SceneLight> lights_;
};
} // namespace Shadow::Scene
