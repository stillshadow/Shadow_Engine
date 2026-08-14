#include "Editor/EditorLayer.h"

#include "Editor/ScenePicking.h"

#include "Assets/AssetManager.h"

#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
const char* LightTypeName(const Shadow::Scene::LightType type) noexcept
{
    switch (type)
    {
    case Shadow::Scene::LightType::Directional:
        return "Directional";
    case Shadow::Scene::LightType::Point:
        return "Point";
    case Shadow::Scene::LightType::Area:
        return "Area";
    case Shadow::Scene::LightType::Spot:
        return "Spot";
    }
    return "Unknown";
}

bool ProjectWorldPoint(
    const DirectX::XMFLOAT3& worldPoint,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection,
    const float viewportWidth,
    const float viewportHeight,
    ImVec2& screenPoint)
{
    using namespace DirectX;
    const XMVECTOR clip = XMVector4Transform(
        XMVectorSet(worldPoint.x, worldPoint.y, worldPoint.z, 1.0F), view * projection);
    const float clipW = XMVectorGetW(clip);
    if (!std::isfinite(clipW) || clipW <= 0.001F)
    {
        return false;
    }

    const float inverseW = 1.0F / clipW;
    const float ndcX = XMVectorGetX(clip) * inverseW;
    const float ndcY = XMVectorGetY(clip) * inverseW;
    const float ndcZ = XMVectorGetZ(clip) * inverseW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || ndcZ < 0.0F || ndcZ > 1.0F)
    {
        return false;
    }
    screenPoint = ImVec2(
        (ndcX * 0.5F + 0.5F) * viewportWidth,
        (-ndcY * 0.5F + 0.5F) * viewportHeight);
    return true;
}

DirectX::XMVECTOR SafeLightDirection(const DirectX::XMFLOAT3& direction)
{
    using namespace DirectX;
    const XMVECTOR value = XMLoadFloat3(&direction);
    const float lengthSquared = XMVectorGetX(XMVector3LengthSq(value));
    return lengthSquared > 0.000001F
        ? XMVector3Normalize(value)
        : XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F);
}

void BuildAreaBasis(
    const DirectX::XMFLOAT3& direction,
    DirectX::XMVECTOR& right,
    DirectX::XMVECTOR& up,
    DirectX::XMVECTOR& normal)
{
    using namespace DirectX;
    normal = SafeLightDirection(direction);
    const XMVECTOR helper = std::abs(XMVectorGetY(normal)) < 0.95F
        ? XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F)
        : XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
    right = XMVector3Normalize(XMVector3Cross(helper, normal));
    up = XMVector3Normalize(XMVector3Cross(normal, right));
}

DirectX::XMMATRIX AreaOrientationFromDirection(const DirectX::XMFLOAT3& direction)
{
    using namespace DirectX;
    const XMVECTOR base = XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F);
    const XMVECTOR target = SafeLightDirection(direction);
    const float cosine = std::clamp(XMVectorGetX(XMVector3Dot(base, target)), -1.0F, 1.0F);
    const XMVECTOR axis = XMVector3Cross(base, target);
    const float axisLengthSquared = XMVectorGetX(XMVector3LengthSq(axis));
    if (axisLengthSquared < 0.000001F)
    {
        return cosine > 0.0F
            ? XMMatrixIdentity()
            : XMMatrixRotationX(XM_PI);
    }
    return XMMatrixRotationAxis(
        XMVector3Normalize(axis), std::acos(cosine));
}

bool SameFloat3(
    const DirectX::XMFLOAT3& left,
    const DirectX::XMFLOAT3& right) noexcept
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool SameScene(const Shadow::Scene::Scene& left, const Shadow::Scene::Scene& right)
{
    const auto& leftObjects = left.Objects();
    const auto& rightObjects = right.Objects();
    if (leftObjects.size() != rightObjects.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < leftObjects.size(); ++index)
    {
        const auto& a = leftObjects[index];
        const auto& b = rightObjects[index];
        if (a.name != b.name || a.assetKey != b.assetKey ||
            !SameFloat3(a.transform.position, b.transform.position) ||
            !SameFloat3(a.transform.rotationDegrees, b.transform.rotationDegrees) ||
            !SameFloat3(a.transform.scale, b.transform.scale) ||
            a.material.baseColor.x != b.material.baseColor.x ||
            a.material.baseColor.y != b.material.baseColor.y ||
            a.material.baseColor.z != b.material.baseColor.z ||
            a.material.baseColor.w != b.material.baseColor.w ||
            a.material.roughness != b.material.roughness ||
            a.material.metallic != b.material.metallic ||
            a.material.normalStrength != b.material.normalStrength ||
            a.material.parallaxHeightScale != b.material.parallaxHeightScale)
        {
            return false;
        }
    }

    const auto& leftEnvironment = left.Environment();
    const auto& rightEnvironment = right.Environment();
    if (leftEnvironment.intensity != rightEnvironment.intensity ||
        leftEnvironment.rotationDegrees != rightEnvironment.rotationDegrees ||
        leftEnvironment.exposure != rightEnvironment.exposure ||
        leftEnvironment.iblSpecularStrength != rightEnvironment.iblSpecularStrength ||
        leftEnvironment.ssaoStrength != rightEnvironment.ssaoStrength ||
        leftEnvironment.bloomEnabled != rightEnvironment.bloomEnabled ||
        leftEnvironment.msaaEnabled != rightEnvironment.msaaEnabled ||
        leftEnvironment.showGeometryNormals != rightEnvironment.showGeometryNormals ||
        leftEnvironment.bloomThreshold != rightEnvironment.bloomThreshold ||
        leftEnvironment.bloomStrength != rightEnvironment.bloomStrength ||
        leftEnvironment.renderPath != rightEnvironment.renderPath)
    {
        return false;
    }

    const auto& leftLights = left.Lights();
    const auto& rightLights = right.Lights();
    if (leftLights.size() != rightLights.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < leftLights.size(); ++index)
    {
        const auto& a = leftLights[index];
        const auto& b = rightLights[index];
        if (a.name != b.name || a.type != b.type ||
            !SameFloat3(a.direction, b.direction) ||
            !SameFloat3(a.position, b.position) ||
            !SameFloat3(a.color, b.color) ||
            a.intensity != b.intensity || a.range != b.range ||
            a.width != b.width || a.height != b.height ||
            a.innerConeDegrees != b.innerConeDegrees ||
            a.outerConeDegrees != b.outerConeDegrees)
        {
            return false;
        }
    }
    return true;
}
} // namespace

namespace Shadow::Editor
{
EditorLayer::EditorLayer(std::filesystem::path scenePath) : scenePath_(std::move(scenePath))
{
}

void EditorLayer::LoadInitialScene(Scene::Scene& scene)
{
    RefreshSceneFiles();
    if (std::filesystem::exists(scenePath_))
    {
        LoadScene(scene);
        return;
    }

    // 文件不存在时只在内存中使用默认场景，不自动写回磁盘。
    // 这样用户手动删除一个场景后，重启编辑器不会因为“启动初始化”把它重新创建出来。
    sceneDirty_ = true;
    statusMessage_ = "No saved scene; using in-memory defaults. Press Ctrl+S to save.";
}

void EditorLayer::ToggleNormalDebugView() noexcept
{
    debugViewMode_ = debugViewMode_ == DebugViewMode::WorldNormal
        ? DebugViewMode::Lit
        : DebugViewMode::WorldNormal;
}

void EditorLayer::Draw(
    Scene::Scene& scene,
    Assets::AssetManager& assets,
    const float viewportWidth,
    const float viewportHeight)
{
    ImGuizmo::BeginFrame();
    const Scene::Scene sceneBeforeFrame = scene;
    suppressUndoCapture_ = false;
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput)
    {
        // Tab 是全局编辑器显示开关：隐藏时不绘制面板、光源图形和任何 Gizmo，
        // 这样可以快速得到一张没有编辑器叠加层的干净渲染画面。
        if (ImGui::IsKeyPressed(ImGuiKey_Tab))
        {
            showEditorUi_ = !showEditorUi_;
            // Tab 保留显示/隐藏编辑器叠加层的原行为，同时清空选择与高亮。
            // 选择不是 Scene 数据，因此不会产生撤销记录，也不需要保存场景。
            selectionKind_ = SelectionKind::None;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W))
        {
            gizmoOperation_ = GizmoOperation::Translate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E))
        {
            gizmoOperation_ = GizmoOperation::Rotate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R))
        {
            gizmoOperation_ = GizmoOperation::Scale;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        {
            SaveScene(scene);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        {
            UndoLastSceneChange(scene);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) &&
            selectionKind_ == SelectionKind::Object && !scene.Objects().empty())
        {
            FocusSelectedObject(scene, assets);
        }
    }

    camera_.UpdateInput(!io.WantCaptureMouse && !ImGuizmo::IsUsing());
    const float aspectRatio = viewportWidth / viewportHeight;
    const DirectX::XMMATRIX view = camera_.ViewMatrix();
    const DirectX::XMMATRIX projection = camera_.ProjectionMatrix(aspectRatio);
    if (showEditorUi_)
    {
        DrawScenePanel(scene);
        DrawLightVisuals(scene, view, projection, viewportWidth, viewportHeight);

        // Gizmo 始终绘制；鼠标位于面板上时只关闭交互，避免它因为输入保护而消失。
        // WantCaptureMouse 是 ImGui 给宿主程序的正式输入所有权信号；Hover 判断补足当前帧，
        // 避免 Slider、Combo Popup 等控件的点击穿透到 Viewport 或 Gizmo。
        const bool mouseOwnedByEditor = io.WantCaptureMouse ||
            ImGui::IsAnyItemHovered() ||
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
        const bool allowGizmoInteraction =
            !mouseOwnedByEditor || ImGuizmo::IsUsing();
        DrawGizmo(
            scene, view, projection, viewportWidth, viewportHeight, allowGizmoInteraction);
    }

    // 左键只在空白 Viewport 中触发拾取。面板与 Gizmo 拥有更高输入优先级，
    // 避免拖动轴或点击按钮时意外切换选中物体。
    const bool mouseOwnedByEditor = io.WantCaptureMouse ||
        ImGui::IsAnyItemHovered() ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    if (showEditorUi_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !mouseOwnedByEditor &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
    {
        // 光源图标画在场景物体之上，因此拾取时也优先检查光源；否则位于模型前的图标
        // 会被模型包围盒抢走点击。
        const std::optional<std::size_t> pickedLight = showEditorUi_
            ? PickSceneLight(
                scene, io.MousePos.x, io.MousePos.y,
                viewportWidth, viewportHeight, view, projection)
            : std::nullopt;
        if (pickedLight.has_value())
        {
            selectedLightIndex_ = *pickedLight;
            selectionKind_ = SelectionKind::Light;
            gizmoOperation_ = GizmoOperation::Translate;
        }
        else
        {
            const std::optional<std::size_t> pickedObject = PickSceneObject(
                scene, assets, io.MousePos.x, io.MousePos.y,
                viewportWidth, viewportHeight, view, projection);
            if (pickedObject.has_value())
            {
                selectedObjectIndex_ = *pickedObject;
                selectionKind_ = SelectionKind::Object;
            }
        }
    }

    TrackSceneChange(sceneBeforeFrame, scene);
}

void EditorLayer::DrawScenePanel(Scene::Scene& scene)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float edgePadding = 16.0F;
    constexpr float leftPanelWidth = 300.0F;
    constexpr float rightPanelWidth = 390.0F;
    const float panelHeight = std::max(520.0F, viewport->WorkSize.y - edgePadding * 2.0F);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    // 左侧只放“选择”和“创建”，让中间的渲染画面保持完整可见。
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + edgePadding, viewport->WorkPos.y + edgePadding),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(leftPanelWidth, panelHeight), ImGuiCond_Always);
    if (ImGui::Begin("Scene Browser", nullptr, flags))
    {
        DrawSceneObjectList(scene);
        DrawSceneLightList(scene);
        ImGui::SeparatorText("Status");
        if (ImGui::Button("Hide Editor UI (Tab)", ImVec2(-1.0F, 0.0F)))
        {
            showEditorUi_ = false;
        }
        ImGui::BeginDisabled(!undoScene_.has_value());
        if (ImGui::Button("Undo Last Scene Change (Ctrl+Z)", ImVec2(-1.0F, 0.0F)))
        {
            UndoLastSceneChange(scene);
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s%s", sceneDirty_ ? "Unsaved changes. " : "", statusMessage_.c_str());
        ImGui::TextDisabled("LMB Pick | MMB Orbit | Shift+MMB Pan | Wheel Zoom");
    }
    ImGui::End();

    // 右侧只放当前选中项的参数和场景文件操作，内容过长时由窗口自身滚动。
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - rightPanelWidth - edgePadding,
               viewport->WorkPos.y + edgePadding),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(rightPanelWidth, panelHeight), ImGuiCond_Always);
    if (ImGui::Begin("Inspector", nullptr, flags))
    {
        auto& objects = scene.Objects();
        if (objects.empty())
        {
            ImGui::TextUnformatted("No objects in the scene.");
        }
        else if (selectionKind_ == SelectionKind::Object)
        {
            selectedObjectIndex_ = std::min(selectedObjectIndex_, objects.size() - 1);
            Scene::SceneObject& object = objects[selectedObjectIndex_];
            ImGui::SeparatorText("Selected Object");
            ImGui::Text("Name: %s", object.name.c_str());
            ImGui::TextWrapped("Mesh: %s", object.assetKey.c_str());
            ImGui::SeparatorText("Transform");
            bool transformChanged = false;
            transformChanged |= ImGui::DragFloat3(
                "Position", &object.transform.position.x, 0.02F, -100.0F, 100.0F, "%.2f");
            transformChanged |= ImGui::DragFloat3(
                "Rotation", &object.transform.rotationDegrees.x,
                0.25F, -360.0F, 360.0F, "%.1f deg");
            transformChanged |= ImGui::DragFloat3(
                "Scale", &object.transform.scale.x, 0.01F, 0.01F, 20.0F, "%.2f");
            if (transformChanged)
            {
                object.transform.scale.x = std::max(object.transform.scale.x, 0.01F);
                object.transform.scale.y = std::max(object.transform.scale.y, 0.01F);
                object.transform.scale.z = std::max(object.transform.scale.z, 0.01F);
                sceneDirty_ = true;
            }

            ImGui::TextUnformatted("Gizmo");
            ImGui::SameLine();
            if (ImGui::RadioButton("Move (W)", gizmoOperation_ == GizmoOperation::Translate))
            {
                gizmoOperation_ = GizmoOperation::Translate;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (E)", gizmoOperation_ == GizmoOperation::Rotate))
            {
                gizmoOperation_ = GizmoOperation::Rotate;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (R)", gizmoOperation_ == GizmoOperation::Scale))
            {
                gizmoOperation_ = GizmoOperation::Scale;
            }

            ImGui::SeparatorText("PBR Material");
            float baseColor[3] = {
                object.material.baseColor.x,
                object.material.baseColor.y,
                object.material.baseColor.z,
            };
            if (ImGui::ColorEdit3("Base Color", baseColor))
            {
                object.material.baseColor = {
                    baseColor[0], baseColor[1], baseColor[2],
                    object.material.baseColor.w};
                sceneDirty_ = true;
            }
            sceneDirty_ |= ImGui::SliderFloat(
                "Opacity", &object.material.baseColor.w, 0.05F, 1.0F, "%.2f");
            ImGui::TextDisabled("Opacity < 1 uses the sorted Forward transparent pass");
            sceneDirty_ |= ImGui::SliderFloat(
                "Roughness", &object.material.roughness, 0.0F, 1.0F, "%.2f");
            sceneDirty_ |= ImGui::SliderFloat(
                "Metallic", &object.material.metallic, 0.0F, 1.0F, "%.2f");
            sceneDirty_ |= ImGui::SliderFloat(
                "Normal Strength", &object.material.normalStrength, 0.0F, 2.0F, "%.2f");
            sceneDirty_ |= ImGui::SliderFloat(
                "Parallax Height", &object.material.parallaxHeightScale,
                0.0F, 0.1F, "%.3f");

            constexpr const char* debugViewNames[] = {
                "Lit", "Base Color", "World Normal", "Roughness", "Metallic",
                "IBL Diffuse", "IBL Specular", "Shadow Factor",
                "GBuffer Base Color", "GBuffer Normal", "GBuffer Position", "SSAO"};
            int debugView = static_cast<int>(debugViewMode_);
            if (ImGui::Combo("Debug View", &debugView, debugViewNames, 12))
            {
                debugViewMode_ = static_cast<DebugViewMode>(debugView);
            }
        }
        else if (selectionKind_ == SelectionKind::Light)
        {
            ImGui::SeparatorText("Selected Light");
            ImGui::TextUnformatted("Use the light list on the left to edit this light.");
        }
        else
        {
            ImGui::SeparatorText("No Selection");
            ImGui::TextDisabled("Click an object or light in the viewport/list.");
        }

        ImGui::SeparatorText("Newport Loft HDRI");
        const char* renderPathNames[] = {"Forward", "Deferred"};
        int renderPath = static_cast<int>(scene.Environment().renderPath);
        if (ImGui::Combo("Render Path", &renderPath, renderPathNames, 2))
        {
            scene.Environment().renderPath = static_cast<Scene::RenderPath>(renderPath);
            sceneDirty_ = true;
        }
        if (scene.Environment().renderPath == Scene::RenderPath::Forward)
        {
            ImGui::TextDisabled("Forward: each Mesh Draw performs PBR lighting; GBuffer is skipped.");
        }
        else
        {
            ImGui::TextDisabled("Deferred: GBuffer -> SSAO -> fullscreen PBR lighting.");
        }
        sceneDirty_ |= ImGui::SliderFloat(
            "IBL Intensity", &scene.Environment().intensity, 0.0F, 8.0F, "%.2f");
        sceneDirty_ |= ImGui::SliderFloat(
            "IBL Specular Strength", &scene.Environment().iblSpecularStrength,
            0.0F, 1.0F, "%.2f");
        ImGui::TextDisabled("Controls environment reflection without dimming the sky");
        sceneDirty_ |= ImGui::SliderFloat(
            "Environment Rotation", &scene.Environment().rotationDegrees,
            -180.0F, 180.0F, "%.1f deg");
        sceneDirty_ |= ImGui::SliderFloat(
            "Exposure", &scene.Environment().exposure, 0.1F, 8.0F, "%.2f");
        ImGui::BeginDisabled(scene.Environment().renderPath == Scene::RenderPath::Forward);
        sceneDirty_ |= ImGui::SliderFloat(
            "SSAO Strength", &scene.Environment().ssaoStrength, 0.0F, 1.0F, "%.2f");
        ImGui::EndDisabled();
        if (scene.Environment().renderPath == Scene::RenderPath::Forward)
        {
            ImGui::TextDisabled("SSAO is a Deferred-only Lit pass in the current comparison.");
        }
        sceneDirty_ |= ImGui::Checkbox(
            "Bloom", &scene.Environment().bloomEnabled);
        sceneDirty_ |= ImGui::SliderFloat(
            "Bloom Threshold", &scene.Environment().bloomThreshold, 0.0F, 8.0F, "%.2f");
        sceneDirty_ |= ImGui::SliderFloat(
            "Bloom Strength", &scene.Environment().bloomStrength, 0.0F, 2.0F, "%.2f");
        ImGui::BeginDisabled(scene.Environment().renderPath == Scene::RenderPath::Deferred);
        sceneDirty_ |= ImGui::Checkbox("4x MSAA", &scene.Environment().msaaEnabled);
        ImGui::EndDisabled();
        if (scene.Environment().renderPath == Scene::RenderPath::Deferred)
        {
            ImGui::TextDisabled("MSAA is a Forward-only comparison option");
        }
        sceneDirty_ |= ImGui::Checkbox(
            "Geometry Shader Normals", &scene.Environment().showGeometryNormals);
        ImGui::TextDisabled("Visualizes triangle normals on the selected object");

        if (selectionKind_ == SelectionKind::Light)
        {
            DrawLightsPanel(scene);
        }
        else
        {
            ImGui::SeparatorText("Light Selection");
            ImGui::TextDisabled("Select a light from the left list to edit its parameters.");
        }

        ImGui::SeparatorText("Scene File");
        ImGui::Text("Current: %s", scenePath_.filename().string().c_str());
        if (!sceneFiles_.empty())
        {
            selectedSceneFileIndex_ = std::min(
                selectedSceneFileIndex_, sceneFiles_.size() - 1);
            const std::string selectedFileName =
                sceneFiles_[selectedSceneFileIndex_].filename().string();
            if (ImGui::BeginCombo("Saved Scenes", selectedFileName.c_str()))
            {
                for (std::size_t index = 0; index < sceneFiles_.size(); ++index)
                {
                    const bool selected = index == selectedSceneFileIndex_;
                    const std::string fileName = sceneFiles_[index].filename().string();
                    if (ImGui::Selectable(fileName.c_str(), selected))
                    {
                        selectedSceneFileIndex_ = index;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Load Selected Scene", ImVec2(-1.0F, 0.0F)))
            {
                scenePath_ = sceneFiles_[selectedSceneFileIndex_];
                LoadScene(scene);
            }
        }
        if (selectionKind_ == SelectionKind::Object && !objects.empty() &&
            ImGui::Button("Duplicate Selected", ImVec2(-1.0F, 0.0F)))
        {
            Scene::SceneObject duplicate = objects[selectedObjectIndex_];
            duplicate.name += " Copy";
            duplicate.transform.position.x += 1.0F;
            objects.push_back(std::move(duplicate));
            selectedObjectIndex_ = objects.size() - 1;
            selectionKind_ = SelectionKind::Object;
            sceneDirty_ = true;
        }
        if (selectionKind_ == SelectionKind::Object && objects.size() > 1 &&
            ImGui::Button("Delete Selected", ImVec2(-1.0F, 0.0F)))
        {
            objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(selectedObjectIndex_));
            selectedObjectIndex_ = std::min(selectedObjectIndex_, objects.size() - 1);
            selectionKind_ = SelectionKind::Object;
            sceneDirty_ = true;
        }
        if (ImGui::Button("Save Current Scene (Ctrl+S)", ImVec2(-1.0F, 0.0F)))
        {
            SaveScene(scene);
        }
        ImGui::InputText("New Scene Name", newSceneName_.data(), newSceneName_.size());
        if (ImGui::Button("Save As New Scene", ImVec2(-1.0F, 0.0F)))
        {
            SaveSceneAs(scene);
        }
        if (ImGui::Button("Restore Defaults", ImVec2(-1.0F, 0.0F)))
        {
            scene.ResetToDefaults();
            selectedObjectIndex_ = 0;
            selectedLightIndex_ = 0;
            selectionKind_ = SelectionKind::Object;
            sceneDirty_ = true;
            statusMessage_ = "Defaults restored. Save to keep them.";
        }
        ImGui::TextDisabled("W/E/R Gizmo | F Focus | N Normal debug");
    }
    ImGui::End();
}

void EditorLayer::DrawSceneObjectList(Scene::Scene& scene)
{
    ImGui::SeparatorText("Scene Objects");
    auto& objects = scene.Objects();

    // 这些按钮只创建 SceneObject，真正的 Mesh 数据由 AssetManager 在启动时提供。
    // 因此它们和导入 GLB 一样，都会经过同一套 Transform、材质、拾取和渲染流程。
    const auto addPrimitive = [&](const char* name, const char* assetKey,
                                  const DirectX::XMFLOAT3& position,
                                  const DirectX::XMFLOAT4& color)
    {
        Scene::SceneObject object;
        object.name = name;
        object.assetKey = assetKey;
        object.transform.position = position;
        object.material.baseColor = color;
        object.material.roughness = 0.55F;
        object.material.metallic = 0.0F;
        objects.push_back(std::move(object));
        selectedObjectIndex_ = objects.size() - 1;
        selectionKind_ = SelectionKind::Object;
        sceneDirty_ = true;
    };
    if (ImGui::Button("Add Sphere"))
    {
        addPrimitive(
            "Sphere", "builtin://sphere",
            {1.5F, 1.0F, 0.0F}, {0.20F, 0.45F, 0.95F, 1.0F});
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Cube"))
    {
        addPrimitive(
            "Cube", "builtin://cube",
            {-1.5F, 1.0F, 0.0F}, {0.85F, 0.32F, 0.12F, 1.0F});
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Plane"))
    {
        addPrimitive(
            "Plane", "builtin://plane",
            {0.0F, 0.05F, 1.8F}, {0.28F, 0.32F, 0.38F, 1.0F});
    }
    if (objects.empty())
    {
        ImGui::TextUnformatted("No objects in the scene.");
        return;
    }

    selectedObjectIndex_ = std::min(selectedObjectIndex_, objects.size() - 1);
    if (ImGui::BeginListBox("##SceneObjectList", ImVec2(-1.0F, 220.0F)))
    {
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            const bool selected = selectionKind_ == SelectionKind::Object &&
                index == selectedObjectIndex_;
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(objects[index].name.c_str(), selected))
            {
                selectedObjectIndex_ = index;
                selectionKind_ = SelectionKind::Object;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
    ImGui::TextDisabled("Objects: %zu", objects.size());
}

void EditorLayer::DrawSceneLightList(Scene::Scene& scene)
{
    ImGui::SeparatorText("Lights");
    auto& lights = scene.Lights();
    const std::size_t editableLightCount = static_cast<std::size_t>(std::count_if(
        lights.begin(), lights.end(), [](const Scene::SceneLight& light) {
            return light.type != Scene::LightType::Directional;
        }));
    if (editableLightCount == 0)
    {
        ImGui::TextUnformatted("No Point, Area, or Spot lights in the scene.");
    }
    else
    {
        if (ImGui::BeginListBox("##SceneLightList", ImVec2(-1.0F, 220.0F)))
        {
            for (std::size_t index = 0; index < lights.size(); ++index)
            {
                if (lights[index].type == Scene::LightType::Directional)
                {
                    continue;
                }
                const bool selected = selectionKind_ == SelectionKind::Light &&
                    index == selectedLightIndex_;
                const std::string label =
                    lights[index].name + " (" + LightTypeName(lights[index].type) + ")";
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    selectedLightIndex_ = index;
                    selectionKind_ = SelectionKind::Light;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
    }

    if (ImGui::Button("Add Point", ImVec2(-1.0F, 0.0F)))
    {
        if (lights.size() < 8)
        {
            lights.push_back({
                "Point " + std::to_string(editableLightCount + 1),
                Scene::LightType::Point,
                {0.0F, -1.0F, 0.0F},
                {0.0F, 2.0F, 1.0F},
                {1.0F, 0.65F, 0.35F},
                12.0F,
                8.0F,
                1.0F,
                1.0F,
            });
            selectedLightIndex_ = lights.size() - 1;
            selectionKind_ = SelectionKind::Light;
            sceneDirty_ = true;
        }
    }
    if (ImGui::Button("Add Area", ImVec2(-1.0F, 0.0F)))
    {
        if (lights.size() < 8)
        {
            lights.push_back({
                "Area " + std::to_string(editableLightCount + 1),
                Scene::LightType::Area,
                {0.0F, -1.0F, 0.0F},
                {-1.5F, 3.5F, 1.0F},
                {0.35F, 0.55F, 1.0F},
                8.0F,
                10.0F,
                3.0F,
                2.0F,
            });
            selectedLightIndex_ = lights.size() - 1;
            selectionKind_ = SelectionKind::Light;
            sceneDirty_ = true;
        }
    }
    if (ImGui::Button("Add Spot", ImVec2(-1.0F, 0.0F)))
    {
        if (lights.size() < 8)
        {
            Scene::SceneLight spot;
            spot.name = "Spot " + std::to_string(editableLightCount + 1);
            spot.type = Scene::LightType::Spot;
            spot.direction = {0.0F, -1.0F, 0.0F};
            spot.position = {0.0F, 3.5F, 1.5F};
            spot.color = {1.0F, 0.82F, 0.55F};
            spot.intensity = 18.0F;
            spot.range = 12.0F;
            spot.innerConeDegrees = 20.0F;
            spot.outerConeDegrees = 30.0F;
            lights.push_back(std::move(spot));
            selectedLightIndex_ = lights.size() - 1;
            selectionKind_ = SelectionKind::Light;
            sceneDirty_ = true;
        }
    }

    if (lights.size() >= 8)
    {
        ImGui::TextDisabled("Light limit reached: 8.");
    }
    const bool selectedEditableLight = !lights.empty() &&
        selectedLightIndex_ < lights.size() &&
        lights[selectedLightIndex_].type != Scene::LightType::Directional;
    if (selectedEditableLight)
    {
        if (ImGui::Button("Delete Selected Light", ImVec2(-1.0F, 0.0F)))
        {
            lights.erase(lights.begin() + static_cast<std::ptrdiff_t>(selectedLightIndex_));
            selectedLightIndex_ = 0;
            selectionKind_ = SelectionKind::Light;
            sceneDirty_ = true;
        }
    }
}

void EditorLayer::DrawLightVisuals(
    const Scene::Scene& scene,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection,
    const float viewportWidth,
    const float viewportHeight)
{
    using namespace DirectX;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const auto project = [&](const XMFLOAT3& point, ImVec2& screenPoint) {
        return ProjectWorldPoint(
            point, view, projection, viewportWidth, viewportHeight, screenPoint);
    };
    const auto worldPoint = [](const XMVECTOR value) {
        XMFLOAT3 result{};
        XMStoreFloat3(&result, value);
        return result;
    };

    for (std::size_t index = 0; index < scene.Lights().size(); ++index)
    {
        const Scene::SceneLight& light = scene.Lights()[index];
        // Directional Light 继续参与渲染和阴影，但不属于可编辑的场景光源 UI。
        if (light.type == Scene::LightType::Directional)
        {
            continue;
        }
        const bool selected = selectionKind_ == SelectionKind::Light &&
            index == selectedLightIndex_;
        const ImU32 color = selected ? IM_COL32(255, 245, 110, 255) : IM_COL32(255, 180, 60, 220);
        const ImU32 dimColor = selected ? IM_COL32(255, 245, 110, 90) : IM_COL32(255, 180, 60, 70);

        if (light.type == Scene::LightType::Point)
        {
            ImVec2 center{};
            if (!project(light.position, center))
            {
                continue;
            }
            drawList->AddCircleFilled(center, selected ? 7.0F : 5.0F, color);
            drawList->AddCircle(center, selected ? 12.0F : 9.0F, color, 16, 1.5F);
            drawList->AddLine(
                ImVec2(center.x - 14.0F, center.y),
                ImVec2(center.x + 14.0F, center.y), color, 1.0F);
            drawList->AddLine(
                ImVec2(center.x, center.y - 14.0F),
                ImVec2(center.x, center.y + 14.0F), color, 1.0F);

            ImVec2 rangePoint{};
            const XMVECTOR rangeWorld = XMVectorAdd(
                XMLoadFloat3(&light.position), XMVectorSet(light.range, 0.0F, 0.0F, 0.0F));
            if (project(worldPoint(rangeWorld), rangePoint))
            {
                const float radius = std::clamp(
                    std::abs(rangePoint.x - center.x), 12.0F, 180.0F);
                drawList->AddCircle(center, radius, dimColor, 32, 1.0F);
            }
        }
        else if (light.type == Scene::LightType::Area)
        {
            XMVECTOR right{};
            XMVECTOR up{};
            XMVECTOR normal{};
            BuildAreaBasis(light.direction, right, up, normal);
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
            std::array<ImVec2, 4> screenCorners{};
            bool visible = true;
            for (std::size_t cornerIndex = 0; cornerIndex < corners.size(); ++cornerIndex)
            {
                visible &= project(worldPoint(corners[cornerIndex]), screenCorners[cornerIndex]);
            }
            if (!visible)
            {
                continue;
            }
            drawList->AddQuadFilled(
                screenCorners[0], screenCorners[1], screenCorners[2], screenCorners[3], dimColor);
            for (std::size_t cornerIndex = 0; cornerIndex < screenCorners.size(); ++cornerIndex)
            {
                drawList->AddLine(
                    screenCorners[cornerIndex],
                    screenCorners[(cornerIndex + 1) % screenCorners.size()],
                    color, selected ? 3.0F : 2.0F);
            }
            ImVec2 centerScreen{};
            const XMVECTOR normalEnd = XMVectorAdd(center, XMVectorScale(normal, 1.25F));
            if (project(worldPoint(center), centerScreen))
            {
                ImVec2 normalScreen{};
                if (project(worldPoint(normalEnd), normalScreen))
                {
                    drawList->AddLine(centerScreen, normalScreen, color, selected ? 3.0F : 2.0F);
                }
            }
        }
        else
        {
            ImVec2 centerScreen{};
            if (!project(light.position, centerScreen))
            {
                continue;
            }
            const XMVECTOR center = XMLoadFloat3(&light.position);
            const XMVECTOR direction = SafeLightDirection(light.direction);
            XMVECTOR right{};
            XMVECTOR up{};
            XMVECTOR normal{};
            BuildAreaBasis(light.direction, right, up, normal);
            const float coneLength = std::min(light.range, 4.0F);
            const float coneRadius = std::tan(XMConvertToRadians(light.outerConeDegrees)) *
                coneLength;
            const XMVECTOR coneCenter = XMVectorAdd(center, XMVectorScale(direction, coneLength));
            const std::array<XMVECTOR, 4> rimPoints = {
                XMVectorAdd(coneCenter, XMVectorScale(right, coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(right, -coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(up, coneRadius)),
                XMVectorAdd(coneCenter, XMVectorScale(up, -coneRadius)),
            };
            drawList->AddCircleFilled(centerScreen, selected ? 7.0F : 5.0F, color);
            for (const XMVECTOR rimPoint : rimPoints)
            {
                ImVec2 rimScreen{};
                if (project(worldPoint(rimPoint), rimScreen))
                {
                    drawList->AddLine(centerScreen, rimScreen, color, selected ? 3.0F : 2.0F);
                }
            }
        }
    }
}

void EditorLayer::DrawLightsPanel(Scene::Scene& scene)
{
    ImGui::SeparatorText("Selected Light Parameters");
    auto& lights = scene.Lights();
    const bool selectedEditableLight = !lights.empty() &&
        selectedLightIndex_ < lights.size() &&
        lights[selectedLightIndex_].type != Scene::LightType::Directional;
    if (!selectedEditableLight)
    {
        ImGui::TextUnformatted("Select a Point, Area, or Spot light from the left list.");
    }
    else
    {
        Scene::SceneLight& light = lights[selectedLightIndex_];
        ImGui::Text("Type: %s", LightTypeName(light.type));
        float color[3] = {light.color.x, light.color.y, light.color.z};
        if (ImGui::ColorEdit3("Light Color", color))
        {
            light.color = {color[0], color[1], color[2]};
            sceneDirty_ = true;
        }
        sceneDirty_ |= ImGui::DragFloat(
            "Intensity", &light.intensity, 0.05F, 0.0F, 100.0F, "%.2f");

        if (light.type == Scene::LightType::Point)
        {
            sceneDirty_ |= ImGui::DragFloat3(
                "Light Position", &light.position.x, 0.05F, -100.0F, 100.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat(
                "Range", &light.range, 0.05F, 0.1F, 100.0F, "%.2f");
            ImGui::TextDisabled("Select this light and use Move (W) to drag it in the viewport.");
        }
        else if (light.type == Scene::LightType::Area)
        {
            sceneDirty_ |= ImGui::DragFloat3(
                "Light Position", &light.position.x, 0.05F, -100.0F, 100.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat3(
                "Emission Direction", &light.direction.x, 0.02F, -1.0F, 1.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat(
                "Range", &light.range, 0.05F, 0.1F, 100.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat(
                "Width", &light.width, 0.02F, 0.01F, 20.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat(
                "Height", &light.height, 0.02F, 0.01F, 20.0F, "%.2f");
            ImGui::TextDisabled("W moves the area light; E rotates its emission direction.");
        }
        else
        {
            sceneDirty_ |= ImGui::DragFloat3(
                "Light Position", &light.position.x, 0.05F, -100.0F, 100.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat3(
                "Emission Direction", &light.direction.x, 0.02F, -1.0F, 1.0F, "%.2f");
            sceneDirty_ |= ImGui::DragFloat(
                "Range", &light.range, 0.05F, 0.1F, 100.0F, "%.2f");
            sceneDirty_ |= ImGui::SliderFloat(
                "Inner Cone", &light.innerConeDegrees, 1.0F, 88.0F, "%.1f deg");
            light.outerConeDegrees = std::max(
                light.outerConeDegrees, light.innerConeDegrees);
            sceneDirty_ |= ImGui::SliderFloat(
                "Outer Cone", &light.outerConeDegrees,
                light.innerConeDegrees, 89.0F, "%.1f deg");
            ImGui::TextDisabled("W moves the spot light; E rotates its emission direction.");
        }
    }

}

void EditorLayer::DrawGizmo(
    Scene::Scene& scene,
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& projection,
    const float viewportWidth,
    const float viewportHeight,
    const bool allowInteraction)
{
    if (selectionKind_ == SelectionKind::None)
    {
        return;
    }
    // 物体和光源共用同一个 ImGuizmo，但两者的可编辑内容不同：
    // 物体可以移动/旋转/缩放，Point Light 只移动，Area/Spot Light 可以移动和旋转。
    const bool lightSelected = selectionKind_ == SelectionKind::Light &&
        !scene.Lights().empty();
    if (lightSelected &&
        scene.Lights()[std::min(selectedLightIndex_, scene.Lights().size() - 1)].type ==
            Scene::LightType::Directional)
    {
        // Directional Light 没有位置，选中它时不显示位置 Gizmo，也不会误操作当前物体。
        return;
    }
    const bool editingLight = lightSelected;
    Scene::Transform* objectTransform = nullptr;
    Scene::SceneLight* sceneLight = nullptr;
    DirectX::XMMATRIX gizmoModel = DirectX::XMMatrixIdentity();
    if (editingLight)
    {
        sceneLight = &scene.Lights()[std::min(selectedLightIndex_, scene.Lights().size() - 1)];
        const bool hasDirectionGizmo = sceneLight->type == Scene::LightType::Area ||
            sceneLight->type == Scene::LightType::Spot;
        const DirectX::XMMATRIX orientation = hasDirectionGizmo
            ? AreaOrientationFromDirection(sceneLight->direction)
            : DirectX::XMMatrixIdentity();
        gizmoModel = orientation * DirectX::XMMatrixTranslation(
            sceneLight->position.x, sceneLight->position.y, sceneLight->position.z);
    }
    else
    {
        if (scene.Objects().empty())
        {
            return;
        }
        selectedObjectIndex_ = std::min(selectedObjectIndex_, scene.Objects().size() - 1);
        objectTransform = &scene.Objects()[selectedObjectIndex_].transform;
        gizmoModel = Scene::TransformMatrix(*objectTransform);
    }

    DirectX::XMFLOAT4X4 viewForGizmo{};
    DirectX::XMFLOAT4X4 projectionForGizmo{};
    DirectX::XMFLOAT4X4 modelForGizmo{};
    // DirectXMath 的行向量矩阵在内存中与 ImGuizmo 需要的列主序数组互为同一表示，
    // 因此这里直接复制；额外转置反而会把平移从索引 12 移到错误位置。
    DirectX::XMStoreFloat4x4(&viewForGizmo, view);
    DirectX::XMStoreFloat4x4(&projectionForGizmo, projection);
    DirectX::XMStoreFloat4x4(&modelForGizmo, gizmoModel);

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0F, 0.0F, viewportWidth, viewportHeight);
    ImGuizmo::Enable(allowInteraction);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    const bool rotatingDirectedLight = editingLight && sceneLight != nullptr &&
        (sceneLight->type == Scene::LightType::Area ||
         sceneLight->type == Scene::LightType::Spot) &&
        gizmoOperation_ == GizmoOperation::Rotate;
    if ((!editingLight || rotatingDirectedLight) && gizmoOperation_ == GizmoOperation::Rotate)
    {
        operation = ImGuizmo::ROTATE;
    }
    else if (!editingLight && gizmoOperation_ == GizmoOperation::Scale)
    {
        operation = ImGuizmo::SCALE;
    }

    if (ImGuizmo::Manipulate(
            &viewForGizmo._11,
            &projectionForGizmo._11,
            operation,
            ImGuizmo::LOCAL,
            &modelForGizmo._11))
    {
        float position[3]{};
        float rotation[3]{};
        float scale[3]{};
        ImGuizmo::DecomposeMatrixToComponents(
            &modelForGizmo._11, position, rotation, scale);
        if (sceneLight != nullptr)
        {
            sceneLight->position = {position[0], position[1], position[2]};
            if (rotatingDirectedLight)
            {
                const DirectX::XMMATRIX eulerRotation = DirectX::XMMatrixRotationRollPitchYaw(
                    DirectX::XMConvertToRadians(rotation[0]),
                    DirectX::XMConvertToRadians(rotation[1]),
                    DirectX::XMConvertToRadians(rotation[2]));
                DirectX::XMFLOAT3 direction{};
                DirectX::XMStoreFloat3(
                    &direction,
                    DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(
                        DirectX::XMVectorSet(0.0F, -1.0F, 0.0F, 0.0F), eulerRotation)));
                sceneLight->direction = direction;
            }
        }
        else if (objectTransform != nullptr)
        {
            objectTransform->position = {position[0], position[1], position[2]};
            objectTransform->rotationDegrees = {rotation[0], rotation[1], rotation[2]};
            objectTransform->scale = {
                std::max(scale[0], 0.01F),
                std::max(scale[1], 0.01F),
                std::max(scale[2], 0.01F),
            };
        }
        sceneDirty_ = true;
    }
}

void EditorLayer::SaveScene(const Scene::Scene& scene)
{
    if (Scene::SceneSerializer::Save(scenePath_, scene, statusMessage_))
    {
        sceneDirty_ = false;
        statusMessage_ = "Saved: " + scenePath_.string();
        RefreshSceneFiles();
    }
}

void EditorLayer::SaveSceneAs(const Scene::Scene& scene)
{
    std::string name(newSceneName_.data());
    const std::size_t firstCharacter = name.find_first_not_of(" \t");
    const std::size_t lastCharacter = name.find_last_not_of(" \t");
    if (firstCharacter == std::string::npos)
    {
        statusMessage_ = "Scene name cannot be empty.";
        return;
    }
    name = name.substr(firstCharacter, lastCharacter - firstCharacter + 1);
    constexpr std::string_view forbiddenCharacters = "<>:\"/\\|?*";
    if (name == "." || name == ".." ||
        name.find_first_of(forbiddenCharacters) != std::string::npos)
    {
        statusMessage_ = "Scene name contains unsupported path characters.";
        return;
    }
    constexpr std::string_view extension = ".scene.json";
    if (name.ends_with(extension))
    {
        name.resize(name.size() - extension.size());
    }
    if (name.empty())
    {
        statusMessage_ = "Scene name cannot be empty.";
        return;
    }
    const std::filesystem::path newPath =
        scenePath_.parent_path() / (name + std::string(extension));
    if (std::filesystem::exists(newPath))
    {
        statusMessage_ = "Scene already exists; load it and use Save Current Scene.";
        return;
    }
    if (Scene::SceneSerializer::Save(newPath, scene, statusMessage_))
    {
        scenePath_ = newPath;
        sceneDirty_ = false;
        statusMessage_ = "Created scene: " + scenePath_.string();
        RefreshSceneFiles();
    }
}

void EditorLayer::RefreshSceneFiles()
{
    sceneFiles_.clear();
    const std::filesystem::path directory = scenePath_.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file())
        {
            continue;
        }
        const std::string fileName = iterator->path().filename().string();
        if (fileName.ends_with(".scene.json"))
        {
            sceneFiles_.push_back(iterator->path());
        }
    }
    std::ranges::sort(sceneFiles_);
    const auto current = std::ranges::find(sceneFiles_, scenePath_);
    selectedSceneFileIndex_ = current == sceneFiles_.end()
        ? 0 : static_cast<std::size_t>(std::distance(sceneFiles_.begin(), current));
}

void EditorLayer::LoadScene(Scene::Scene& scene)
{
    if (Scene::SceneSerializer::Load(scenePath_, scene, statusMessage_))
    {
        selectedObjectIndex_ = std::min(selectedObjectIndex_, scene.Objects().size() - 1);
        selectedLightIndex_ = 0;
        selectionKind_ = SelectionKind::Object;
        sceneDirty_ = false;
        statusMessage_ = "Loaded: " + scenePath_.string();
    }
}

void EditorLayer::UndoLastSceneChange(Scene::Scene& scene)
{
    if (!undoScene_.has_value())
    {
        return;
    }

    scene = std::move(*undoScene_);
    undoScene_.reset();
    undoTransactionActive_ = false;
    suppressUndoCapture_ = true;
    selectedObjectIndex_ = scene.Objects().empty()
        ? 0 : std::min(selectedObjectIndex_, scene.Objects().size() - 1);
    selectedLightIndex_ = scene.Lights().empty()
        ? 0 : std::min(selectedLightIndex_, scene.Lights().size() - 1);
    sceneDirty_ = true;
    statusMessage_ = "Undid the last scene change.";
}

void EditorLayer::TrackSceneChange(
    const Scene::Scene& sceneBeforeFrame,
    const Scene::Scene& sceneAfterFrame)
{
    if (suppressUndoCapture_)
    {
        return;
    }

    const bool sceneChanged = !SameScene(sceneBeforeFrame, sceneAfterFrame);
    const bool continuousInteraction =
        ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
    if (sceneChanged)
    {
        if (!undoTransactionActive_)
        {
            undoScene_ = sceneBeforeFrame;
        }
        // Slider/Drag/Gizmo 按住期间保持同一个事务；松开后下一次修改会建立新快照。
        undoTransactionActive_ = continuousInteraction;
    }
    else if (!continuousInteraction)
    {
        undoTransactionActive_ = false;
    }
}

void EditorLayer::FocusSelectedObject(
    const Scene::Scene& scene, const Assets::AssetManager& assets) noexcept
{
    const Scene::SceneObject& object = scene.Objects()[selectedObjectIndex_];
    const float maximumScale = std::max({
        object.transform.scale.x,
        object.transform.scale.y,
        object.transform.scale.z,
    });
    const Assets::MeshAsset* mesh = assets.FindMesh(object.assetKey);
    const DirectX::XMFLOAT3 extents = mesh != nullptr
        ? mesh->boundsExtents : DirectX::XMFLOAT3{1.0F, 1.0F, 1.0F};
    const float localRadius = std::max({extents.x, extents.y, extents.z});
    camera_.Focus(object.transform.position, maximumScale * localRadius);
}
} // namespace Shadow::Editor
