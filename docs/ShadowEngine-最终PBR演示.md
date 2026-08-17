# 最终 PBR 演示：Emissive BoomBox

当前默认场景是一个可复现的单模型材质演示：地面使用内置网格，主体使用 Khronos glTF Sample Assets 的 `BoomBox.glb`。这个 CC0 GLB 只有一个 Mesh 和一个材质，并将 base color、normal、metallic-roughness 与 emissive 纹理嵌在同一个文件中，可以同时验证 Cook-Torrance PBR、自发光 HDR 数据和 Bloom。

## 运行后应该检查什么

1. 先确认主体不再是两个相互遮挡的默认球体。
2. 按 `Tab` 隐藏左右编辑器面板和 Gizmo，查看干净的渲染结果。
3. 按 `D` 快切 `Lit → GBuffer Base Color → GBuffer Normal → GBuffer Position → SSAO → Lit`。
4. 调整 `Newport Loft HDRI` 的 Intensity 与 Rotation，确认天空和金属反射同步变化；使用 `IBL Specular Strength` 单独控制模型环境反射而不压暗天空。
5. 调整 `Emissive Strength`，再切换 Bloom，确认前面板先写入线性 HDR、经过亮部提取和模糊后才叠加回最终画面。
6. 切换 `Render Path` 的 Forward / Deferred，确认两条路径都保留相同的自发光结果。

## HDRI 数据如何流进画面

```text
newport_loft.hdr（经纬度图）
        │  HdrEnvironment.cpp：CPU 双线性采样
        ▼
D3D12 TextureCube（+X/-X/+Y/-Y/+Z/-Z）
        ├─ Skybox.hlsl：画出相机背景
        ├─ MaterialPreview.hlsl：Forward PBR 环境光
        └─ DeferredLighting.hlsl：Deferred PBR 环境光
```

三条路径读取同一个 SRV，因此旋转环境时，天空、Forward 和 Deferred 的环境方向保持一致。
当前教学版用少量运行时采样近似 Irradiance 与 Roughness Prefilter；完整商业实现通常会预计算
Irradiance Cubemap、GGX Prefilter Mip Chain 和 BRDF LUT，减少每像素采样并提高物理准确性。

## 资产位置

```text
assets/models/BoomBox/BoomBox.glb
assets/models/BoomBox/README.md
assets/scenes/current.scene.json
assets/environments/NewportLoft/newport_loft.hdr
assets/environments/NewportLoft/README.md
```

场景中的 `assetKey` 指向 GLB 的第一个 primitive：

```text
gltf://assets/models/BoomBox/BoomBox.glb#mesh=0/primitive=0
```

如果替换模型，保持“单个 Mesh、单个材质、纹理嵌入 GLB”的约束即可；启动时 `AssetManager::DiscoverGltfAssets` 会自动扫描 `assets/models/`，无需在 ImGui 中手动导入。

## 许可证提醒

该演示资产来自 Khronos 的 glTF Sample Assets，由 Microsoft 于 2017 年以 CC0-1.0 发布，可以用于公开作品集；仓库仍保留来源与许可记录，方便审核和复现。
