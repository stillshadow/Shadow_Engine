# Shadow Engine 作品集验收说明

## 最终展示内容

默认场景使用一个自包含、CC0 的 `BoomBox.glb` Emissive PBR 资产和内置 Ground。它不依赖 Blender 文件或绝对路径，打开引擎即可复现。建议录制一段 30～60 秒视频，按下面顺序展示：

1. `Lit`：展示最终材质和光照。
2. `GBuffer Normal / Position`：说明延迟渲染如何把几何信息写入 MRT。
3. 按 `D` 快切核心 Debug View，再拖动 SSAO Strength，观察接触与凹角遮蔽。
4. 调整 Emissive Strength，关闭/打开 Bloom，改变 Exposure 和 Threshold，展示自发光如何进入 HDR 后处理。
5. 改变 Normal Strength、Parallax Height、Roughness、Metallic，展示材质参数如何沿 CPU → Constant Buffer → HLSL 流动。
6. 一键切换 Forward / Deferred，并展示 Deferred 的四张 GBuffer（含 Emissive）与 SSAO。

## 面试时可以清楚回答的问题

- 为什么先写 GBuffer，再做光照？因为材质和几何数据只生成一次，后续光照/屏幕空间效果可以复用它们。
- 为什么需要资源屏障？D3D12 不替驱动隐式追踪资源用途；同一资源在 RTV、SRV、UAV 之间切换前必须声明状态。
- 为什么 HDR 不能直接写到普通 UNORM？高光能量可能超过 1，应该在线性 HDR 中保留，再用曝光和 Tone Mapping 压缩到显示范围。
- SSAO 为什么放在 GBuffer 之后？它需要法线和位置；当前版本使用 View Space 深度与 16 个确定性屏幕邻域采样，真实项目通常还会加入噪声旋转、半球 Kernel 和双边模糊。
- 当前 IBL 和完整商业引擎有什么差异？这里已读取 Newport Loft HDRI 并转换为真实 TextureCube，但 Irradiance 和 Roughness Prefilter 仍用运行时多点采样近似；商业实现通常预计算 irradiance、GGX prefilter mip 和 BRDF LUT。
- Forward 和 Deferred 的核心差异是什么？Forward 在每个物体 Draw 中直接计算光照；Deferred 先保存可见表面，再用全屏 Pass 统一光照，以额外显存和带宽换取更灵活的屏幕空间复用与多灯光扩展。

## 验收命令

```powershell
cmake --build --preset debug
ctest --test-dir build\vscode -C Debug --output-on-failure
```

运行文件：`build/vscode/Debug/ShadowEngine.exe`。

## 已知边界

当前资产导入仍以单 mesh/单材质 GLB 为教学约束；第一盏方向光和第一盏点光源生成阴影，面光源与聚光灯暂不生成独立阴影贴图；离线 IBL 预计算、完整生产级 SSAO 和商业资产管线属于后续扩展，不影响本作品集展示两条光照路径、HLSL 和可检查 Pass 的主目标。
