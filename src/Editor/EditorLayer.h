#pragma once

#include "Editor/EditorCamera.h"
#include "Scene/Scene.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Shadow::Assets
{
class AssetManager;
}

namespace Shadow::Editor
{
enum class DebugViewMode : std::uint32_t
{
    Lit = 0,
    BaseColor = 1,
    WorldNormal = 2,
    Roughness = 3,
    Metallic = 4,
    IblDiffuse = 5,
    IblSpecular = 6,
    ShadowFactor = 7,
    GBufferBaseColor = 8,
    GBufferNormal = 9,
    GBufferPosition = 10,
    Ssao = 11,
};

class EditorLayer final
{
public:
    explicit EditorLayer(std::filesystem::path scenePath);

    void LoadInitialScene(Scene::Scene& scene);
    void Draw(
        Scene::Scene& scene, Assets::AssetManager& assets,
        float viewportWidth, float viewportHeight);
    void ToggleNormalDebugView() noexcept;
    void CyclePortfolioDebugViews() noexcept;

    [[nodiscard]] DebugViewMode DebugView() const noexcept { return debugViewMode_; }
    [[nodiscard]] std::size_t SelectedObjectIndex() const noexcept { return selectedObjectIndex_; }
    [[nodiscard]] bool IsObjectSelected() const noexcept
    {
        return selectionKind_ == SelectionKind::Object;
    }
    [[nodiscard]] bool ShowViewportOverlays() const noexcept
    {
        return !hideViewportOverlays_;
    }
    [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept { return camera_.ViewMatrix(); }
    [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix(float aspectRatio) const noexcept
    {
        return camera_.ProjectionMatrix(aspectRatio);
    }

private:
    enum class GizmoOperation
    {
        Translate,
        Rotate,
        Scale,
    };

    enum class SelectionKind
    {
        None,
        Object,
        Light,
    };

    void DrawScenePanel(Scene::Scene& scene);
    void DrawSceneObjectList(Scene::Scene& scene);
    void DrawSceneLightList(Scene::Scene& scene);
    void DrawLightsPanel(Scene::Scene& scene);
    void DeleteSelectedLight(Scene::Scene& scene);
    void DrawLightVisuals(
        const Scene::Scene& scene,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& projection,
        float viewportWidth,
        float viewportHeight);
    void DrawGizmo(
        Scene::Scene& scene,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& projection,
        float viewportWidth,
        float viewportHeight,
        bool allowInteraction);
    void SaveScene(const Scene::Scene& scene);
    void LoadScene(Scene::Scene& scene);
    void SaveSceneAs(const Scene::Scene& scene);
    void RefreshSceneFiles();
    void FocusSelectedObject(
        const Scene::Scene& scene, const Assets::AssetManager& assets) noexcept;
    void UndoLastSceneChange(Scene::Scene& scene);
    void TrackSceneChange(
        const Scene::Scene& sceneBeforeFrame, const Scene::Scene& sceneAfterFrame);

    std::filesystem::path scenePath_;
    std::vector<std::filesystem::path> sceneFiles_;
    std::size_t selectedSceneFileIndex_ = 0;
    std::array<char, 64> newSceneName_{"Scene"};
    EditorCamera camera_;
    std::size_t selectedObjectIndex_ = 0;
    std::size_t selectedLightIndex_ = 0;
    SelectionKind selectionKind_ = SelectionKind::Object;
    GizmoOperation gizmoOperation_ = GizmoOperation::Translate;
    DebugViewMode debugViewMode_ = DebugViewMode::Lit;
    bool showEditorUi_ = true;
    bool hideViewportOverlays_ = false;
    bool sceneDirty_ = false;
    // 只保存一个 Scene 快照：连续拖动视为一次操作，下一次新操作会覆盖旧快照。
    std::optional<Scene::Scene> undoScene_;
    bool undoTransactionActive_ = false;
    bool suppressUndoCapture_ = false;
    std::string statusMessage_ = "Scene loaded from defaults.";
    // 只记录 AssetManager 已发现的资源；文件导入职责属于 Blender 导出与启动扫描。
    // 保存“模型文件”的 URI，而不是某个 Primitive 的 key，避免多材质模型在列表中重复出现。
};
} // namespace Shadow::Editor
