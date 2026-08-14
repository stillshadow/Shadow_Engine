# Shadow Engine 技术美术导读

这份导读现在围绕“可检查的混合实时渲染器”组织，不再包含已经取消的旧材质效果或完整街角场景路线。

## 作品集目标

Shadow Engine 不是要替代 Unreal Engine，而是用一套足够小、足够透明的 C++/DX12 工程，展示你能解释并手写：

- CPU 如何准备 Scene、材质、相机和灯光数据；
- GPU 如何经过 Forward 或 GBuffer/Deferred Lighting、阴影和后处理多个 Pass；
- HLSL 如何读取资源、计算 PBR，并把反射结果合成回最终颜色；
- 如何用 Debug View、RenderDoc/PIX 和资源状态验证结果。

## 推荐复盘顺序

1. `src/Scene/Scene.h`：确认场景数据只描述对象、材质、灯光和环境。
2. `src/Editor/EditorLayer.cpp`：确认 ImGui/Gizmo 修改的是 CPU 场景，不直接碰 GPU 资源。
3. `D3D12Renderer::UpdateObjectConstants`：确认每个对象如何获得模型矩阵、相机、灯光和调试参数。
4. `D3D12Renderer::Render`：对比 Forward 直接光照与 GBuffer → Deferred Lighting 的分支。
5. `shaders/GBuffer.hlsl`：理解 MRT 如何把材质、法线和世界坐标拆成可复用的纹理。
6. `shaders/DeferredLighting.hlsl`：理解屏幕空间像素如何从 GBuffer 恢复材质并统一光照。
7. `shaders/MaterialPreview.hlsl`：理解 Forward Pixel Shader 如何直接计算同一套 PBR。

## 面试表达模板

“我用同一套场景数据和 Cook-Torrance PBR 实现了两条路径。Forward 在物体 Draw 的 Pixel Shader 中直接计算灯光；Deferred 先写 BaseColor、Normal、Roughness、Metallic 和 WorldPosition，再通过全屏 Pass 统一光照。两条路径共享 HDRI、阴影和后处理，可以在编辑器中一键切换并检查中间结果。”

详细的执行顺序、资源状态和 Unreal Engine 对照见 `ShadowEngine-混合渲染器垂直切片.md`。
